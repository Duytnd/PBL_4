#include "network/torrent_session.h"

#include "core/hash_utils.h"
#include "network/allowed_fast.h"
#include "network/extension.h"
#include "network/lan_address.h"
#include "network/lsd.h"
#include "network/nat_traversal.h"
#include "network/network_monitor.h"
#include "network/udp_rendezvous.h"
#include "network/utp_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>

namespace p2p::network {

namespace {

std::string generate_azureus_peer_id(const std::string& prefix = "-PB0001-") {
    std::string id = prefix;
    static constexpr char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(charset) - 2);

    while (id.size() < 20) {
        id.push_back(charset[dist(gen)]);
    }
    return id;
}

std::string session_state_to_string(SessionState s) {
    switch (s) {
        case SessionState::Stopped: return "stopped";
        case SessionState::Checking: return "checking";
        case SessionState::Downloading: return "downloading";
        case SessionState::Waiting: return "waiting";
        case SessionState::Seeding: return "seeding";
        case SessionState::Paused: return "paused";
        case SessionState::Error: return "error";
    }
    return "unknown";
}

}  // namespace

namespace {

// true nếu tracker nằm trong LAN (IP private hoặc loopback). Announce rỗng (chỉ DHT) thì xét IP máy này.
// Tên miền không biết được là LAN hay không -> coi là ngoài Internet.
bool announce_targets_lan(const std::string& announce_url) {
    if (announce_url.empty()) {
        return KademliaNode::is_private_ipv4(NetworkMonitor::instance().primary_ip());
    }
    const auto scheme_end = announce_url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::string rest = announce_url.substr(scheme_end + 3);
    std::string host = rest.substr(0, rest.find_first_of(":/"));
    return KademliaNode::is_private_ipv4(host) || network::is_loopback_address(host);
}

}  // namespace

TorrentSession::TorrentSession(
    const TorrentFileMetadata& metadata,
    const TorrentSessionConfig& config)
    : metadata_(metadata),
            config_(config),
            advertised_port_(config.listen_port),
            download_limiter_(config.max_download_rate),
            upload_limiter_(config.max_upload_rate) {
    if (config_.client_peer_id.empty()) {
        peer_id_ = generate_azureus_peer_id();
    } else {
        peer_id_ = config_.client_peer_id;
        peer_id_.resize(20, '0');
    }

    info_hash_raw_ = p2p::sha1Digest(metadata_.infoRaw);
    info_hash_hex_ = p2p::sha1Hex(metadata_.infoRaw);

    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const std::uint64_t key = gen();
        announce_key_raw_.assign(8, '\0');
        static const char* kHex = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) {
            const auto byte = static_cast<unsigned char>((key >> (8 * (7 - i))) & 0xFF);
            announce_key_raw_[static_cast<std::size_t>(i)] = static_cast<char>(byte);
            announce_key_hex_.push_back(kHex[byte >> 4]);
            announce_key_hex_.push_back(kHex[byte & 0x0F]);
        }
    }

    save_path_ = config_.download_dir / metadata_.name;
    part_path_ = save_path_;
    part_path_ += ".part";

    total_bytes_ = metadata_.length;
    if (total_bytes_ == 0 && !metadata_.pieces.empty()) {
        total_bytes_ = static_cast<std::uint64_t>(metadata_.pieceLength) * (metadata_.pieces.size() - 1);
    }
    left_bytes_ = total_bytes_;

    const std::size_t piece_count = metadata_.pieces.size();
    my_bitfield_.assign(piece_count, false);
    piece_wanted_.assign(piece_count, true);
    file_wanted_.assign(metadata_.isMultiFile() ? metadata_.files.size() : 1, true);
    wanted_remaining_ = piece_count;
    wanted_bytes_ = total_bytes_;

    piece_manager_ = std::make_unique<PieceManager>(
        piece_count,
        static_cast<std::size_t>(metadata_.pieceLength),
        static_cast<std::size_t>(total_bytes_),
        metadata_.pieces);

    // Mặc định sử dụng chiến lược Rarest-First và Tit-For-Tat
    strategy::RarestFirstConfig picker_config;
    picker_config.pipeline_depth = std::max<std::size_t>(1, config_.pipeline_depth);
    piece_picker_ = std::make_unique<strategy::RarestFirstPicker>(picker_config);
    choker_ = std::make_unique<strategy::TitForTatChoker>();

    check_existing_files();
}

TorrentSession::TorrentSession(
    const std::filesystem::path& torrentFilePath,
    const TorrentSessionConfig& config)
    : TorrentSession(TorrentFile::parseFile(torrentFilePath.string()), config) {}

TorrentSession::~TorrentSession() {
    stop();
    // Announce chạy nền có timeout 5s (TrackerClient), nên chờ này luôn hữu hạn.
    while (announce_inflight_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void TorrentSession::set_state(SessionState new_state) {
    SessionState old_state = state_;
    if (old_state != new_state) {
        state_ = new_state;
        if (state_cb_) {
            state_cb_(old_state, new_state);
        }
    }
}

void TorrentSession::check_existing_files() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    set_state(SessionState::Checking);

    std::cout << "[SESSION] Checking file: " << save_path_
              << " | Expected total: " << total_bytes_ << " bytes, "
              << metadata_.pieces.size() << " pieces" << std::endl;

    // Single-file: file đầy đủ ưu tiên; chưa có thì resume từ file tạm .part.
    // Multi-file: dữ liệu ghi thẳng vào các file dưới thư mục save_path_ (không có .part).
    const bool multi = metadata_.isMultiFile();
    const std::filesystem::path& source =
        (multi || std::filesystem::exists(save_path_)) ? save_path_ : part_path_;
    if (std::filesystem::exists(source)) {
        const bool loaded = multi ? piece_manager_->loadFromFiles(save_path_, metadata_.files)
                                  : piece_manager_->loadFromFile(source);
        if (loaded) {
            completed_pieces_count_ = 0;
            for (std::size_t i = 0; i < metadata_.pieces.size(); ++i) {
                if (piece_manager_->verifyPiece(i)) {
                    my_bitfield_[i] = true;
                    completed_pieces_count_++;
                    const std::size_t psize = piece_manager_->pieceSize(i);
                    if (left_bytes_ >= psize) {
                        left_bytes_ -= psize;
                    } else {
                        left_bytes_ = 0;
                    }
                } else {
                    // Dữ liệu không khớp hash (vùng chưa tải/hỏng): bỏ để tải lại.
                    piece_manager_->discardPiece(i);
                }
            }
            std::cout << "[SESSION] File loaded successfully. Completed pieces: "
                      << completed_pieces_count_ << "/" << metadata_.pieces.size() << std::endl;
        } else {
            std::cout << "[SESSION] Warning: Failed to load existing file into memory." << std::endl;
        }
    } else {
        std::cout << "[SESSION] File does not exist yet (download target): " << save_path_ << std::endl;
    }

    recompute_wanted_locked();
    if (all_wanted_done_locked()) {
        set_state(SessionState::Seeding);
        std::cout << "[SESSION] Status set to SEEDING (100% complete)" << std::endl;
    } else if (completed_pieces_count_ > 0) {
        set_state(SessionState::Downloading);
        std::cout << "[SESSION] Status set to DOWNLOADING (" << completed_pieces_count_ << "/"
                  << metadata_.pieces.size() << " pieces ready, resuming download)" << std::endl;
    } else {
        set_state(SessionState::Stopped);
        std::cout << "[SESSION] Status set to STOPPED (" << completed_pieces_count_ << "/"
                  << metadata_.pieces.size() << " pieces ready)" << std::endl;
    }
}

bool TorrentSession::start() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (running_) {
        return true;
    }

    try {
        acceptor_ioc_ = std::make_unique<asio::io_context>();
        // Không dùng constructor (endpoint) của asio: nó bật SO_REUSEADDR, mà trên Windows
        // cờ này cho phép bind chồng lên port đang có process khác listen, nên start()
        // "thành công" nhưng peer kết nối vào nhầm process kia.
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(*acceptor_ioc_);
        const asio::ip::tcp::endpoint listen_ep(asio::ip::tcp::v4(), config_.listen_port);
        acceptor_->open(listen_ep.protocol());
#ifndef _WIN32
        // POSIX: giữ reuse để khởi động lại nhanh khi socket cũ còn TIME_WAIT.
        acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
#endif
        acceptor_->bind(listen_ep);
        acceptor_->listen();
    } catch (const std::exception& ex) {
        error_message_ = std::string("Failed to bind listen port ") + std::to_string(config_.listen_port) + ": " + ex.what();
        set_state(SessionState::Error);
        std::cout << "[SESSION] " << error_message_ << std::endl;
        return false;
    }

    advertised_port_.store(config_.listen_port);
    tcp_port_mapped_ = false;
    mapped_tcp_port_ = 0;
    nat_mapping_initialization_done_.store(!config_.enable_port_mapping);

    running_ = true;
    paused_ = false;

    if (config_.enable_port_mapping) {
        nat_init_thread_ = std::thread([this]() {
            struct InitGuard {
                std::atomic<bool>& done;
                ~InitGuard() { done.store(true); }
            } guard{nat_mapping_initialization_done_};

            establish_port_mapping();
        });
    }

    // Theo dõi đổi mạng: chỉ đặt cờ (callback chạy trên luồng monitor), việc nặng làm ở orchestration_loop.
    network_changed_ = false;
    network_primary_changed_ = false;
    network_full_refresh_ = false;
    network_subscription_ = NetworkMonitor::instance().subscribe([this](const NetworkChange& change) {
        if (change.primary_changed) {
            network_primary_changed_ = true;
        }
        if (change.primary_changed || change.was_offline) {
            network_full_refresh_ = true;
        }
        network_changed_ = true;
    });

    if (config_.enable_utp && config_.enable_tracker && !metadata_.announce.empty()) {
        try {
            const auto [tracker_host, tracker_port] =
                TrackerClient::udpRendezvousEndpoint(metadata_.announce);
            asio::io_context resolver_ioc;
            asio::ip::udp::resolver resolver(resolver_ioc);
            asio::error_code resolve_ec;
            const auto endpoints = resolver.resolve(
                tracker_host, std::to_string(tracker_port), resolve_ec);
            if (resolve_ec) {
                throw std::runtime_error("tracker UDP resolve failed: " + resolve_ec.message());
            }

            bool found_ipv4 = false;
            for (const auto& entry : endpoints) {
                if (entry.endpoint().address().is_v4()) {
                    tracker_udp_endpoint_ = entry.endpoint();
                    found_ipv4 = true;
                    break;
                }
            }
            if (!found_ipv4) {
                throw std::runtime_error("tracker has no IPv4 UDP endpoint");
            }

            utp_engine_ = std::make_shared<pbl4::network::UtpEngine>();
            utp_engine_->set_accept_handler([this](std::shared_ptr<pbl4::network::UtpStream> stream) {
                accept_utp_stream(std::move(stream));
            });
            utp_engine_->set_datagram_handler(
                [this](const char* data, std::size_t size, const asio::ip::udp::endpoint& source) {
                    return handle_rendezvous_datagram(data, size, source);
                });
            utp_engine_->start();
            next_udp_registration_ = std::chrono::steady_clock::now();
            std::cout << "[uTP] UDP transport started on local port " << utp_engine_->local_port()
                      << "; tracker rendezvous at " << tracker_udp_endpoint_ << std::endl;
        } catch (const std::exception& ex) {
            utp_engine_.reset();
            std::cout << "[uTP] UDP rendezvous unavailable: " << ex.what() << std::endl;
        }
    }

    if (all_wanted_done_locked()) {
        set_state(SessionState::Seeding);
        std::cout << "[SESSION] Started as SEEDER on port " << config_.listen_port << std::endl;
    } else {
        set_state(SessionState::Downloading);
        std::cout << "[SESSION] Started as DOWNLOADER on port " << config_.listen_port << std::endl;
    }

    last_rate_calc_time_ = std::chrono::steady_clock::now();
    last_downloaded_snapshot_ = downloaded_bytes_;
    last_uploaded_snapshot_ = uploaded_bytes_;

    // Khởi động Local Service Discovery (BEP 14) nếu bật
    if (config_.enable_lsd) {
        try {
            lsd_ = std::make_unique<LocalServiceDiscovery>(config_.listen_port);
            lsd_->start([this](const LsdPeer& peer) {
                if (peer.infoHashHex.empty() || peer.infoHashHex == info_hash_hex_) {
                    add_peer(peer.ip, peer.port);
                }
            });
            lsd_->announce(info_hash_raw_);
        } catch (...) {
            // Không ngắt session nếu LSD không bind được multicast
        }
    }

    // Khởi động Distributed Hash Table (BEP 5) nếu bật.
    // Không chạy bootstrap/lookup đồng bộ trong start() vì các thao tác UDP DHT
    // có thể chậm và khóa request tạo torrent.
    if (config_.enable_dht) {
        try {
            if (config_.shared_dht) {
                // Dùng shared DHT node từ StateBridge (tất cả sessions dùng chung 1 node)
                dht_shared_ = config_.shared_dht;
                std::cout << "[DHT] Using shared Kademlia node on UDP port " << dht_shared_->port() << std::endl;

                dht_init_thread_ = std::thread([this]() {
                    if (!running_) return;

                    // Announce bản thân + lấy peers từ DHT
                    auto* d = dht();
                    if (!d) return;

                    if (!config_.dht_bootstrap_host.empty() && config_.dht_bootstrap_port != 0) {
                        d->bootstrap(config_.dht_bootstrap_host, config_.dht_bootstrap_port);
                    }

                    if (!running_) return;

                    const auto dht_peers = d->getPeers(info_hash_raw_);
                    for (const auto& peer : dht_peers) {
                        if (running_) {
                            add_peer(peer.ip, peer.port);
                        }
                    }

                    while (running_ && config_.enable_port_mapping &&
                           !nat_mapping_initialization_done_.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    if (running_) {
                        d->announcePeer(info_hash_raw_, advertised_port_.load());
                    }
                });
            } else {
                // Tạo node DHT riêng cho session này
                const std::uint16_t dport = config_.dht_port != 0 ? config_.dht_port : config_.listen_port;
                dht_ = std::make_unique<KademliaNode>(NodeId::random(), dport);
                if (dht_->start()) {
                    std::cout << "[DHT] Kademlia node started on UDP port " << dht_->port() << std::endl;

                    dht_init_thread_ = std::thread([this]() {
                        if (!running_) return;

                        if (!config_.dht_bootstrap_host.empty() && config_.dht_bootstrap_port != 0) {
                            dht_->bootstrap(config_.dht_bootstrap_host, config_.dht_bootstrap_port);
                        }

                        if (!running_) return;

                        const auto dht_peers = dht_->getPeers(info_hash_raw_);
                        for (const auto& peer : dht_peers) {
                            if (running_) {
                                add_peer(peer.ip, peer.port);
                            }
                        }

                        while (running_ && config_.enable_port_mapping &&
                               !nat_mapping_initialization_done_.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        if (running_) {
                            dht_->announcePeer(info_hash_raw_, advertised_port_.load());
                        }
                    });
                } else {
                    std::cout << "[DHT] Could not bind Kademlia node on port " << dport << ", continuing without DHT" << std::endl;
                    dht_.reset();
                }
            }
        } catch (...) {
            dht_.reset();
            dht_shared_.reset();
        }
    }

    // Khởi động Inbound Acceptor thread
    acceptor_thread_ = std::thread(&TorrentSession::acceptor_loop, this);

    // Khởi động Orchestration thread (Tracker, Choker, Maintenance)
    orchestration_thread_ = std::thread(&TorrentSession::orchestration_loop, this);

    return true;
}

void TorrentSession::stop() {
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
        paused_ = false;
        set_state(SessionState::Stopped);

        if (lsd_) {
            lsd_->stop();
        }

        // Đóng các kết nối peer để giải phóng thread
        for (auto& [ep, peer] : active_peers_) {
            if (peer && peer->connection) {
                peer->connection->close();
            }
        }
    }

    // Ngừng nhận sự kiện đổi mạng (chờ callback đang chạy xong) trước khi huỷ bất cứ thứ gì.
    if (network_subscription_ != 0) {
        NetworkMonitor::instance().unsubscribe(network_subscription_);
        network_subscription_ = 0;
    }

    if (dht_init_thread_.joinable()) {
        dht_init_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (dht_) {
            dht_->stop();
        }
        // shared_dht không stop ở đây — do StateBridge quản lý lifecycle
        dht_shared_.reset();
    }

    // Đóng Inbound Acceptor ngay lập tức
    if (acceptor_) {
        asio::error_code ec;
        acceptor_->close(ec);
    }
    if (acceptor_ioc_) {
        acceptor_ioc_->stop();
    }
    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }

    // Thông báo sự kiện dừng tới Tracker
    process_tracker_announce("stopped");

    // Đợi Orchestration thread
    if (orchestration_thread_.joinable()) {
        orchestration_thread_.join();
    }

    if (nat_init_thread_.joinable()) {
        nat_init_thread_.join();
    }

    if (utp_engine_) {
        utp_engine_->stop();
    }

    // Đợi và dọn dẹp các worker threads của peer
    std::unordered_map<std::string, std::shared_ptr<ActivePeer>> peers_to_join;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        peers_to_join = active_peers_;
    }

    for (auto& [ep, peer] : peers_to_join) {
        if (peer && peer->worker_thread.joinable()) {
            peer->worker_thread.join();
        }
    }

    if (nat_traversal_ && tcp_port_mapped_) {
        try {
            if (nat_traversal_->unmap_port(mapped_tcp_port_, pbl4::network::NatProtocol::TCP)) {
                std::cout << "[NAT] Removed TCP peer port mapping " << mapped_tcp_port_ << std::endl;
            } else {
                std::cout << "[NAT] Could not remove TCP peer port mapping " << mapped_tcp_port_ << std::endl;
            }
        } catch (const std::exception& ex) {
            std::cout << "[NAT] Failed to remove TCP peer port mapping: " << ex.what() << std::endl;
        }
        tcp_port_mapped_ = false;
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        active_peers_.clear();
        in_progress_pieces_.clear();
        pending_blocks_.clear();
        lsd_.reset();
        nat_traversal_.reset();
        utp_engine_.reset();
        dht_.reset();
    }
}

void TorrentSession::recompute_wanted_locked() {
    wanted_remaining_ = 0;
    wanted_bytes_ = 0;
    left_bytes_ = 0;
    for (std::size_t i = 0; i < piece_wanted_.size(); ++i) {
        if (!piece_wanted_[i]) {
            continue;
        }
        const std::uint64_t size = piece_manager_->pieceSize(i);
        wanted_bytes_ += size;
        if (!my_bitfield_[i]) {
            ++wanted_remaining_;
            left_bytes_ += size;
        }
    }
}

bool TorrentSession::set_file_selection(const std::vector<bool>& wanted) {
    std::unique_lock<std::mutex> lock(session_mutex_);
    bool need_peers = false;
    if (wanted.size() != file_wanted_.size() ||
        std::none_of(wanted.begin(), wanted.end(), [](bool w) { return w; })) {
        return false;
    }

    std::vector<bool> piece_wanted(piece_wanted_.size(), false);
    if (!metadata_.isMultiFile()) {
        piece_wanted.assign(piece_wanted.size(), true);
    } else {
        const std::uint64_t piece_len = static_cast<std::uint64_t>(metadata_.pieceLength);
        std::uint64_t offset = 0;
        for (std::size_t f = 0; f < metadata_.files.size(); ++f) {
            const std::uint64_t length = metadata_.files[f].length;
            if (wanted[f] && length > 0 && piece_len > 0) {
                const std::size_t first = static_cast<std::size_t>(offset / piece_len);
                const std::size_t last = static_cast<std::size_t>((offset + length - 1) / piece_len);
                for (std::size_t p = first; p <= last && p < piece_wanted.size(); ++p) {
                    piece_wanted[p] = true;
                }
            }
            offset += length;
        }
    }

    file_wanted_ = wanted;
    piece_wanted_ = std::move(piece_wanted);
    recompute_wanted_locked();

    // Bỏ các piece dở dang không còn được chọn (đã nhận một phần): giải phóng chỗ trong pipeline.
    for (auto it = in_progress_pieces_.begin(); it != in_progress_pieces_.end();) {
        if (it->first < piece_wanted_.size() && !piece_wanted_[it->first]) {
            const std::uint32_t dropped = static_cast<std::uint32_t>(it->first);
            pending_blocks_.erase(
                std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                               [dropped](const auto& pb) { return pb.piece_index == dropped; }),
                pending_blocks_.end());
            it = in_progress_pieces_.erase(it);
        } else {
            ++it;
        }
    }

    if (running_ && !paused_) {
        if (all_wanted_done_locked()) {
            set_state(SessionState::Seeding);
        } else {
            if (state_ == SessionState::Seeding) {
                set_state(SessionState::Downloading);
                need_peers = true;  // kết nối tới các seed khác thường đã rụng khi mình xong việc
            }
            need_peers = need_peers || active_peers_.empty();
            // Peer đang kết nối có thể vừa trở nên đáng quan tâm (piece mới được chọn).
            for (auto& [endpoint, peer] : active_peers_) {
                if (!peer || !peer->connection || peer->am_interested || !peer->connection->is_open()) {
                    continue;
                }
                for (std::size_t i = 0; i < peer->bitfield.size() && i < my_bitfield_.size(); ++i) {
                    if (peer->bitfield[i] && !my_bitfield_[i] && piece_wanted_[i]) {
                        try {
                            peer->connection->send_interested();
                            peer->am_interested = true;
                        } catch (...) {
                        }
                        break;
                    }
                }
            }
        }
    }
    lock.unlock();
    // Lấy lại peer ngay thay vì chờ chu kỳ tracker. Dọn peer đã chết trước, nếu không announce sẽ bỏ qua
    // chúng vì "đã biết" (vòng bảo trì chỉ dọn mỗi giây một lần).
    if (need_peers) {
        cleanup_disconnected_peers();
        process_tracker_announce("");
    }
    return true;
}

std::vector<bool> TorrentSession::file_selection() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return file_wanted_;
}

std::vector<FileStatus> TorrentSession::files_status() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::vector<FileStatus> result;
    const std::uint64_t piece_len = static_cast<std::uint64_t>(metadata_.pieceLength);

    std::vector<FileEntry> files = metadata_.files;
    if (files.empty()) {
        files.push_back(FileEntry{metadata_.name, total_bytes_});
    }

    std::uint64_t offset = 0;
    for (std::size_t f = 0; f < files.size(); ++f) {
        FileStatus status;
        status.path = files[f].path;
        status.length = files[f].length;
        status.wanted = f < file_wanted_.size() ? static_cast<bool>(file_wanted_[f]) : true;

        if (status.length > 0 && piece_len > 0) {
            const std::uint64_t end = offset + status.length;
            const std::size_t first = static_cast<std::size_t>(offset / piece_len);
            const std::size_t last = static_cast<std::size_t>((end - 1) / piece_len);
            for (std::size_t p = first; p <= last && p < my_bitfield_.size(); ++p) {
                if (!my_bitfield_[p]) {
                    continue;
                }
                const std::uint64_t piece_start = static_cast<std::uint64_t>(p) * piece_len;
                const std::uint64_t piece_end = piece_start + piece_manager_->pieceSize(p);
                const std::uint64_t from = std::max(piece_start, offset);
                const std::uint64_t to = std::min(piece_end, end);
                if (to > from) {
                    status.completed_bytes += to - from;
                }
            }
        }
        offset += status.length;
        result.push_back(std::move(status));
    }
    return result;
}

void TorrentSession::pause() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (running_ && !paused_) {
        paused_ = true;
        set_state(SessionState::Paused);
    }
}

void TorrentSession::resume() {
    bool need_peers = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (running_ && paused_) {
            paused_ = false;
            if (all_wanted_done_locked()) {
                set_state(SessionState::Seeding);
            } else {
                set_state(SessionState::Downloading);
                need_peers = true;
            }
        }
    }
    // Pause làm các worker đóng kết nối và vòng bảo trì tạm dừng nên chưa ai dọn các peer đã chết: dọn ngay,
    // nếu không lượt announce này sẽ thấy chúng "đã biết" và bỏ qua, rồi phải chờ chu kỳ tracker (~60 giây).
    if (need_peers) {
        cleanup_disconnected_peers();
        process_tracker_announce("");
    }
}

bool TorrentSession::is_running() const noexcept {
    return running_;
}

bool TorrentSession::is_complete() const noexcept {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return all_wanted_done_locked();
}

SessionState TorrentSession::state() const noexcept {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return state_;
}

void TorrentSession::add_peer(const std::string& ip, std::uint16_t port) {
    if (ip.empty() || port == 0) return;

    // Tắt DHT thì không có localIp từ DHT: tự dò, tránh local_ip rỗng làm bộ lọc coi mọi peer public là rác.
    std::string local_ip = (dht() && !dht()->localIp().empty()) ? dht()->localIp() : "";
    if (local_ip.empty()) {
        // Không dùng biến static: nó chốt IP của mạng lúc gọi đầu tiên mãi mãi, đổi mạng là lọc peer sai.
        local_ip = NetworkMonitor::instance().primary_ip();
    }
    const char* allow_public_env = std::getenv("P2P_ALLOW_PUBLIC_PEERS");
    const bool allow_public_peers =
        config_.allow_public_peers || (allow_public_env != nullptr && std::string(allow_public_env) == "1");
    bool has_private_alternative = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (const auto& existing : known_peer_endpoints_) {
            const auto colon = existing.find(':');
            if (colon == std::string::npos) continue;
            const std::string existing_ip = existing.substr(0, colon);
            if (KademliaNode::is_private_ipv4(existing_ip)) {
                has_private_alternative = true;
                break;
            }
        }
        if (!KademliaNode::should_connect_peer(local_ip, ip, has_private_alternative, allow_public_peers)) {
            std::cout << "[ADD-PEER] Skipping public peer " << ip << ":" << port
                      << " while LAN private peers are preferred" << std::endl;
            return;
        }

        const std::string ep = ip + ":" + std::to_string(port);
        discovered_tcp_peer_endpoints_.insert(ep);
        bool is_new = false;
        if (known_peer_endpoints_.insert(ep).second) {
            candidate_peer_endpoints_.push_back(ep);
            is_new = true;
        }

        std::cout << "[ADD-PEER] " << ep
                  << (is_new ? " -> added to candidates" : " -> already known, skipped")
                  << " | running=" << running_ << " paused=" << paused_ << std::endl;
    }

    if (running_ && !paused_) {
        connect_candidate_peers();
    }
}

void TorrentSession::send_udp_registration() {
    if (!utp_engine_ || !running_ || tracker_udp_endpoint_.port() == 0 ||
        (config_.enable_port_mapping && !nat_mapping_initialization_done_.load())) {
        return;
    }
    try {
        utp_engine_->send_datagram(
            tracker_udp_endpoint_,
            udp_rendezvous::encode_register_request(
                info_hash_raw_, peer_id_, advertised_port_.load(), announce_key_raw_));
    } catch (const std::exception& ex) {
        std::cout << "[uTP] UDP registration failed: " << ex.what() << std::endl;
    }
}

void TorrentSession::request_peer_holepunch(const std::string& ip, std::uint16_t tcp_port) {
    if (!utp_engine_ || !running_ || tracker_udp_endpoint_.port() == 0 || tcp_port == 0) {
        return;
    }
    const std::string tcp_endpoint = ip + ":" + std::to_string(tcp_port);
    const auto now = std::chrono::steady_clock::now();
    std::uint32_t transaction = 0;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (discovered_tcp_peer_endpoints_.find(tcp_endpoint) == discovered_tcp_peer_endpoints_.end()) {
            return;
        }
        const auto previous = last_rendezvous_request_.find(tcp_endpoint);
        if (previous != last_rendezvous_request_.end() &&
            now - previous->second < std::chrono::seconds(10)) {
            return;
        }
        last_rendezvous_request_[tcp_endpoint] = now;

        // Yêu cầu chờ phản hồi quá 30 giây là không bao giờ có phản hồi nữa: dọn, và chặn kích thước.
        for (auto it = pending_rendezvous_targets_.begin(); it != pending_rendezvous_targets_.end();) {
            it = now - it->second.requested_at > std::chrono::seconds(30) ? pending_rendezvous_targets_.erase(it)
                                                                           : std::next(it);
        }
        if (pending_rendezvous_targets_.size() >= 256) {
            return;
        }
        // transaction_id ngẫu nhiên, khác 0 và không trùng yêu cầu đang chờ.
        static thread_local std::mt19937 transaction_rng{std::random_device{}()};
        do {
            transaction = std::uniform_int_distribution<std::uint32_t>(1, 0xFFFFFFFFu)(transaction_rng);
        } while (pending_rendezvous_targets_.count(transaction) != 0);
        pending_rendezvous_targets_[transaction] = PendingRendezvous{tcp_endpoint, now};
        if (last_rendezvous_request_.size() > 512) {
            for (auto it = last_rendezvous_request_.begin(); it != last_rendezvous_request_.end();) {
                if (now - it->second > std::chrono::minutes(1)) {
                    it = last_rendezvous_request_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    try {
        utp_engine_->send_datagram(
            tracker_udp_endpoint_,
            udp_rendezvous::encode_punch_request(
                info_hash_raw_, peer_id_, advertised_port_.load(), ip, tcp_port, transaction, announce_key_raw_));
        send_udp_registration();
        std::cout << "[uTP] Requested UDP rendezvous for " << tcp_endpoint << std::endl;
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        pending_rendezvous_targets_.erase(transaction);
        std::cout << "[uTP] Hole-punch request failed: " << ex.what() << std::endl;
    }
}

bool TorrentSession::handle_rendezvous_datagram(
    const char* data,
    std::size_t size,
    const asio::ip::udp::endpoint& source) {
    if (!data || size < 4 || std::memcmp(data, "P4UT", 4) != 0) {
        return false;
    }
    if (source != tracker_udp_endpoint_) {
        return true;
    }

    udp_rendezvous::Result result;
    if (udp_rendezvous::decode_punch_result(data, size, result)) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (result.info_hash != info_hash_raw_) {
            return true;
        }
        const auto pending = pending_rendezvous_targets_.find(result.transaction_id);
        if (pending == pending_rendezvous_targets_.end()) {
            return true;
        }
        const std::string expected_tcp_endpoint = pending->second.tcp_endpoint;
        pending_rendezvous_targets_.erase(pending);
        const auto colon = expected_tcp_endpoint.find(':');
        if (result.status == udp_rendezvous::Status::Ok &&
            colon != std::string::npos &&
            result.ip == expected_tcp_endpoint.substr(0, colon)) {
            const std::string endpoint = result.ip + ":" + std::to_string(result.port);
            if (utp_engine_) {
                utp_engine_->allow_peer(asio::ip::udp::endpoint(
                    asio::ip::make_address_v4(result.ip), result.port));
            }
            if (queued_utp_peer_endpoints_.insert(endpoint).second &&
                utp_peer_candidates_.size() < config_.max_connections * 2) {
                utp_peer_candidates_.push_back(endpoint);
            }
        }
        return true;
    }

    udp_rendezvous::Notification notification;
    if (udp_rendezvous::decode_notification(data, size, notification)) {
        if (notification.info_hash != info_hash_raw_) {
            return true;
        }
        const std::string tcp_endpoint = notification.tcp_ip + ":" + std::to_string(notification.tcp_port);
        const std::string endpoint = notification.ip + ":" + std::to_string(notification.port);
        if (utp_engine_) {
            utp_engine_->allow_peer(asio::ip::udp::endpoint(
                asio::ip::make_address_v4(notification.ip), notification.port));
        }
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (discovered_tcp_peer_endpoints_.find(tcp_endpoint) != discovered_tcp_peer_endpoints_.end() &&
            queued_utp_peer_endpoints_.insert(endpoint).second &&
            utp_peer_candidates_.size() < config_.max_connections * 2) {
            utp_peer_candidates_.push_back(endpoint);
        }
        return true;
    }
    return true;
}

void TorrentSession::announce_tracker(const std::string& event) {
    process_tracker_announce(event);
}

TorrentSessionStats TorrentSession::stats() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    TorrentSessionStats s;
    s.state = state_;
    s.state_name = session_state_to_string(state_);
    s.total_bytes = total_bytes_;
    s.downloaded_bytes = downloaded_bytes_;
    s.uploaded_bytes = uploaded_bytes_;
    s.left_bytes = left_bytes_;
    s.wanted_bytes = wanted_bytes_;
    s.total_pieces = metadata_.pieces.size();
    s.completed_pieces = completed_pieces_count_;
    s.connected_peers = active_peers_.size();
    s.total_peers_known = known_peer_endpoints_.size();
    s.dht_nodes_count = dht() ? dht()->routingTable().size() : 0;
    s.download_rate = current_download_rate_;
    s.upload_rate = current_upload_rate_;
    s.error_message = error_message_;

    // Nếu đang Seeding → luôn 100%
    if (state_ == SessionState::Seeding) {
        s.progress_percent = 100.0;
    } else if (wanted_bytes_ > 0 && s.total_pieces > 0) {
        // Byte của piece đã verify + các block đã nhận của piece đang dở, để thanh tiến độ
        // chạy liên tục thay vì nhảy từng piece (piece 256KB trở lên trông như đứng yên).
        std::uint64_t partial = 0;
        for (const auto& [idx, prog] : in_progress_pieces_) {
            if (idx < piece_wanted_.size() && piece_wanted_[idx]) {
                partial += static_cast<std::uint64_t>(prog.received_blocks) * pbl4::network::kStandardBlockSize;
            }
        }
        const std::uint64_t verified = wanted_bytes_ > left_bytes_ ? wanted_bytes_ - left_bytes_ : 0;
        const std::uint64_t done = std::min<std::uint64_t>(verified + partial, wanted_bytes_);
        s.progress_percent = std::clamp(
            (static_cast<double>(done) / static_cast<double>(wanted_bytes_)) * 100.0, 0.0, 100.0);
        if (wanted_remaining_ > 0) {
            s.progress_percent = std::min(s.progress_percent, 99.9);  // 100% chỉ khi đã verify hết
        }
    } else if (all_wanted_done_locked()) {
        s.progress_percent = 100.0;
    } else {
        s.progress_percent = 0.0;
    }
    return s;
}

std::vector<SessionPeerInfo> TorrentSession::get_peers() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::vector<SessionPeerInfo> result;
    result.reserve(active_peers_.size());

    const std::size_t total_pcs = metadata_.pieces.size();
    for (const auto& [ep, peer] : active_peers_) {
        SessionPeerInfo info;
        info.ip = peer->ip;
        info.port = peer->port;
        info.peer_id = peer->remote_peer_id;
        info.am_choking = peer->am_choking;
        info.peer_choking = peer->peer_choking;
        info.am_interested = peer->am_interested;
        info.peer_interested = peer->peer_interested;
        info.download_rate = peer->download_rate;
        info.upload_rate = peer->upload_rate;

        if (total_pcs > 0 && !peer->bitfield.empty()) {
            std::size_t has_count = 0;
            for (bool bit : peer->bitfield) {
                if (bit) has_count++;
            }
            info.progress_percent = (static_cast<double>(has_count) / total_pcs) * 100.0;
        } else {
            info.progress_percent = 0.0;
        }
        result.push_back(info);
    }
    return result;
}

const TorrentFileMetadata& TorrentSession::metadata() const noexcept {
    return metadata_;
}

std::vector<bool> TorrentSession::bitfield() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return my_bitfield_;
}

std::string TorrentSession::info_hash_hex() const {
    return info_hash_hex_;
}

const std::string& TorrentSession::peer_id() const noexcept {
    return peer_id_;
}

std::filesystem::path TorrentSession::save_path() const {
    return save_path_;
}

void TorrentSession::set_piece_picker(std::unique_ptr<strategy::PiecePicker> picker) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (picker) {
        piece_picker_ = std::move(picker);
    }
}

void TorrentSession::set_choker(std::unique_ptr<strategy::Choker> choker) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (choker) {
        choker_ = std::move(choker);
    }
}

void TorrentSession::on_state_changed(StateChangedCallback cb) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    state_cb_ = std::move(cb);
}

void TorrentSession::on_piece_completed(PieceCompletedCallback cb) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    piece_cb_ = std::move(cb);
}

void TorrentSession::on_torrent_finished(TorrentFinishedCallback cb) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    finished_cb_ = std::move(cb);
}

void TorrentSession::establish_port_mapping(bool replace_stale_mapping) {
    try {
        auto nat = std::make_unique<pbl4::network::NatTraversal>();
        if (!nat->discover(std::chrono::milliseconds(600))) {
            std::cout << "[NAT] No UPnP/NAT-PMP gateway found; inbound peer connections may require manual port forwarding"
                      << std::endl;
            return;
        }

        auto mapping = nat->map_port(config_.listen_port, 0, pbl4::network::NatProtocol::TCP);
        if (!mapping.success && replace_stale_mapping) {
            // Đổi card mạng nhưng vẫn cùng router: ánh xạ cũ (trỏ về IP cũ của máy này) còn sống trên router, nhiều
            // router từ chối ghi đè sang IP nội bộ khác (UPnP lỗi 718). Gỡ rồi ánh xạ lại một lần.
            nat->unmap_port(config_.listen_port, pbl4::network::NatProtocol::TCP);
            mapping = nat->map_port(config_.listen_port, 0, pbl4::network::NatProtocol::TCP);
        }
        if (!mapping.success) {
            std::cout << "[NAT] Could not map TCP peer port " << config_.listen_port
                      << ": " << mapping.message << std::endl;
            return;
        }
        if (!running_) {
            nat->unmap_port(mapping.external_port, pbl4::network::NatProtocol::TCP);
            return;
        }

        // Router có thể đổi cổng ngoài. Peer cùng LAN nối thẳng IP private tới cổng nội bộ
        // (socket thật chỉ nghe ở listen_port), nên với tracker/mạng nội bộ phải quảng bá
        // listen_port; cổng ngoài chỉ có nghĩa với peer ngoài Internet.
        if (mapping.external_port != config_.listen_port && announce_targets_lan(metadata_.announce)) {
            std::cout << "[NAT] External port " << mapping.external_port
                      << " differs from listen port " << config_.listen_port
                      << "; LAN tracker detected, advertising the listen port to LAN peers" << std::endl;
        } else {
            advertised_port_.store(mapping.external_port);
        }
        mapped_tcp_port_ = mapping.external_port;
        tcp_port_mapped_ = true;
        next_port_mapping_refresh_ =
            std::chrono::steady_clock::now() + std::chrono::minutes(30);
        nat_traversal_ = std::move(nat);
        std::cout << "[NAT] TCP peer port mapped: " << config_.listen_port
                  << " -> " << advertised_port_.load()
                  << " | external IP=" << mapping.external_ip
                  << " | " << mapping.message << std::endl;
    } catch (const std::exception& ex) {
        std::cout << "[NAT] Port mapping unavailable: " << ex.what() << std::endl;
    }
}

void TorrentSession::remap_ports_after_network_change() {
    // KHÔNG gỡ ánh xạ cũ trên router cũ: sang mạng khác thì router cũ không còn với tới, còn nếu router mới trùng địa chỉ
    // (192.168.1.1 ở cả hai nhà) thì lệnh gỡ sẽ rơi vào router mới. Ánh xạ cũ tự hết hạn (lease 1 giờ).
    // Bỏ hẳn đối tượng NatTraversal cũ vì nó nhớ router cũ và IP nội bộ cũ.
    nat_traversal_.reset();
    tcp_port_mapped_ = false;
    mapped_tcp_port_ = 0;
    advertised_port_.store(config_.listen_port);  // mạng mới chưa có ánh xạ: chỉ quảng bá cổng thật
    establish_port_mapping(/*replace_stale_mapping=*/true);
}

bool TorrentSession::handle_network_change() {
    const bool any_change = network_changed_.exchange(false);
    const bool full_refresh = network_full_refresh_.exchange(false);
    const bool primary_changed = network_primary_changed_.exchange(false);
    if (!any_change && !full_refresh) {
        return false;
    }

    std::cout << "[NETWORK] Change detected: local IP now " << NetworkMonitor::instance().primary_ip()
              << (primary_changed ? " (primary address changed)" : full_refresh ? " (network came back)" : " (secondary adapter)")
              << std::endl;

    // Nhóm multicast LSD gắn với card mạng lúc tham gia: tham gia lại và báo luôn cho máy trong LAN mới.
    if (config_.enable_lsd && lsd_) {
        lsd_->rejoinMulticast();
        lsd_->announce(info_hash_raw_);
    }
    if (!full_refresh) {
        return true;  // chỉ đổi card phụ (VPN, card ảo): kết nối hiện có vẫn dùng được
    }

    if (primary_changed) {
        // IP nội bộ của máy đã đổi: mọi kết nối TCP/uTP đang mở gắn với địa chỉ cũ, chờ timeout 30 giây mới biết chết.
        // Đóng ngay để dựng lại trên mạng mới (tracker/LSD/DHT trả lại peer trong vài giây).
        std::vector<std::shared_ptr<pbl4::network::PeerConnection>> stale_connections;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            for (const auto& entry : active_peers_) {
                if (entry.second && entry.second->connection) {
                    stale_connections.push_back(entry.second->connection);
                }
            }
            // Peer ở dải LAN cũ không còn với tới được, nhưng nếu giữ trong known_peer_endpoints_ thì add_peer tưởng vẫn
            // còn "peer LAN thay thế" và bỏ qua mọi peer public => không tải được ở mạng mới.
            const auto is_old_lan_endpoint = [](const std::string& endpoint) {
                const auto colon = endpoint.rfind(':');
                return colon != std::string::npos && KademliaNode::is_private_ipv4(endpoint.substr(0, colon));
            };
            for (auto it = known_peer_endpoints_.begin(); it != known_peer_endpoints_.end();) {
                it = is_old_lan_endpoint(*it) ? known_peer_endpoints_.erase(it) : std::next(it);
            }
            for (auto it = discovered_tcp_peer_endpoints_.begin(); it != discovered_tcp_peer_endpoints_.end();) {
                it = is_old_lan_endpoint(*it) ? discovered_tcp_peer_endpoints_.erase(it) : std::next(it);
            }
            for (auto it = queued_utp_peer_endpoints_.begin(); it != queued_utp_peer_endpoints_.end();) {
                it = is_old_lan_endpoint(*it) ? queued_utp_peer_endpoints_.erase(it) : std::next(it);
            }
            candidate_peer_endpoints_.erase(
                std::remove_if(candidate_peer_endpoints_.begin(), candidate_peer_endpoints_.end(), is_old_lan_endpoint),
                candidate_peer_endpoints_.end());
            utp_peer_candidates_.erase(
                std::remove_if(utp_peer_candidates_.begin(), utp_peer_candidates_.end(), is_old_lan_endpoint),
                utp_peer_candidates_.end());
        }
        for (const auto& connection : stale_connections) {
            try {
                connection->close();
            } catch (...) {
            }
        }
    }

    // Router mới (hoặc router cũ khởi động lại): ánh xạ cổng cũ không còn đúng.
    if (config_.enable_port_mapping && running_) {
        remap_ports_after_network_change();
    }
    if (!running_) {
        return true;
    }

    // IP/cổng công khai đổi: tracker và máy rendezvous UDP phải biết địa chỉ mới.
    send_udp_registration();
    next_udp_registration_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    if (config_.enable_tracker) {
        process_tracker_announce("");
    }
    return true;
}

void TorrentSession::orchestration_loop() {
    while (running_ && config_.enable_port_mapping &&
           !nat_mapping_initialization_done_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!running_) {
        return;
    }

    if (config_.enable_tracker) {
        process_tracker_announce("started");
    }

    auto last_choker_time = std::chrono::steady_clock::now();
    auto last_tracker_time = std::chrono::steady_clock::now();
    auto last_lsd_time = std::chrono::steady_clock::now();
    auto last_dht_time = std::chrono::steady_clock::now();

    while (running_) {
        if (!paused_) {
            auto now = std::chrono::steady_clock::now();

            // 0. Đổi mạng (WiFi <-> dây, đổi WiFi, có lại mạng): làm lại NAT, báo lại tracker/LSD/DHT.
            if (handle_network_change()) {
                now = std::chrono::steady_clock::now();  // remap NAT có thể mất vài giây
                last_tracker_time = now;                 // vừa announce xong
                last_lsd_time = now;
                last_dht_time = {};                      // announce DHT lại ngay ở lượt này với IP/cổng mới
            }

            if (nat_traversal_ && tcp_port_mapped_ && now >= next_port_mapping_refresh_) {
                try {
                    const auto mapping = nat_traversal_->map_port(
                        config_.listen_port, mapped_tcp_port_, pbl4::network::NatProtocol::TCP);
                    if (mapping.success && mapping.external_port == mapped_tcp_port_) {
                        std::cout << "[NAT] Refreshed TCP peer port mapping " << mapped_tcp_port_ << std::endl;
                        next_port_mapping_refresh_ = now + std::chrono::minutes(30);
                    } else {
                        std::cout << "[NAT] Failed to refresh TCP peer port mapping: "
                                  << mapping.message << std::endl;
                        next_port_mapping_refresh_ = now + std::chrono::minutes(5);
                    }
                } catch (const std::exception& ex) {
                    std::cout << "[NAT] Failed to refresh TCP peer port mapping: "
                              << ex.what() << std::endl;
                    next_port_mapping_refresh_ = now + std::chrono::minutes(5);
                }
            }

            // 1. Cập nhật tốc độ & bảo trì kết nối mỗi giây
            update_rates();
            cleanup_disconnected_peers();
            connect_candidate_peers();
            connect_utp_candidates();

            if (utp_engine_ && now >= next_udp_registration_) {
                send_udp_registration();
                next_udp_registration_ = now + std::chrono::seconds(20);
            }

            // 2. Chạy thuật toán chia băng thông (Tit-for-tat)
            if (now - last_choker_time >= config_.choker_interval) {
                perform_choker_update();
                last_choker_time = now;
            }

            // 3. Announce định kỳ tới Tracker (nếu bật), độc lập với DHT
            if (config_.enable_tracker && now - last_tracker_time >= config_.tracker_interval) {
                process_tracker_announce("");
                last_tracker_time = now;
            }

            // 3b. Đang cần tải mà không còn peer nào (vd. seed đóng kết nối, pause/resume, vừa chọn thêm file):
            // hỏi lại tracker sau tối đa 5 giây thay vì chờ chu kỳ đầy đủ. Giới hạn tần suất để không dồn tracker.
            if (config_.enable_tracker && now - last_tracker_time >= std::chrono::seconds(5)) {
                bool starving = false;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    starving = state_ == SessionState::Downloading && active_peers_.empty() &&
                               !all_wanted_done_locked();
                }
                if (starving) {
                    process_tracker_announce("");
                    last_tracker_time = now;
                }
            }

            // 4. Announce định kỳ LSD (mỗi 60 giây)
            if (config_.enable_lsd && lsd_ && now - last_lsd_time >= std::chrono::seconds(60)) {
                lsd_->announce(info_hash_raw_);
                last_lsd_time = now;
            }

            // 5. Announce và truy vấn định kỳ DHT (mỗi 10 giây nếu chưa có peer, 60 giây nếu đã có)
            {
                auto* d = dht();
                const bool has_peers = [this]() {
                    std::lock_guard<std::mutex> lk(session_mutex_);
                    return !active_peers_.empty();
                }();
                const auto dht_interval = has_peers ? std::chrono::seconds(60) : std::chrono::seconds(10);
                if (config_.enable_dht && d && now - last_dht_time >= dht_interval) {
                    const auto dht_peers = d->getPeers(info_hash_raw_);
                    for (const auto& p : dht_peers) {
                        add_peer(p.ip, p.port);
                    }
                    d->announcePeer(info_hash_raw_, advertised_port_.load());
                    last_dht_time = now;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool TorrentSession::inbound_allowed_locked(const std::string& ip) const {
    if (active_peers_.size() >= config_.max_connections) {
        return false;
    }
    const std::size_t reserved_for_outbound = config_.max_connections / 5;
    std::size_t inbound_total = 0;
    std::size_t inbound_same_ip = 0;
    for (const auto& entry : active_peers_) {
        if (!entry.second || entry.second->outbound) {
            continue;
        }
        ++inbound_total;
        if (entry.second->ip == ip) {
            ++inbound_same_ip;
        }
    }
    if (inbound_total + reserved_for_outbound >= config_.max_connections) {
        return false;
    }
    // Nhiều client thật sau cùng một NAT công cộng dùng chung 1 IP: nâng giới hạn bằng P2P_MAX_INBOUND_PER_IP (0 = tắt).
    std::size_t per_ip_limit = config_.max_inbound_per_ip;
    if (const char* env = std::getenv("P2P_MAX_INBOUND_PER_IP")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0') {
            per_ip_limit = static_cast<std::size_t>(parsed);
        }
    }
    return per_ip_limit == 0 || inbound_same_ip < per_ip_limit;
}

void TorrentSession::acceptor_loop() {
    try {
        while (running_) {
            if (!acceptor_) {
                break;
            }

            asio::error_code ec;
            asio::ip::tcp::socket socket = acceptor_->accept(ec);
            if (ec || !running_) {
                break;
            }

            std::string remote_ip = socket.remote_endpoint().address().to_string();
            std::uint16_t remote_port = socket.remote_endpoint().port();
            std::string ep = remote_ip + ":" + std::to_string(remote_port);

            std::shared_ptr<ActivePeer> peer;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                if (!inbound_allowed_locked(remote_ip)) {
                    socket.close(ec);
                    continue;
                }
                peer = std::make_shared<ActivePeer>();
                peer->ip = remote_ip;
                peer->port = remote_port;
                peer->endpoint_str = ep;
                // Chưa bắt tay thì chỉ cho thời gian ngắn: kết nối im lặng không được giữ chỗ lâu.
                peer->connection = std::make_shared<pbl4::network::PeerConnection>(
                    std::move(socket), std::min(config_.handshake_timeout, config_.peer_timeout));
                peer->bitfield.assign(metadata_.pieces.size(), false);
                peer->last_active = std::chrono::steady_clock::now();
                peer->worker_active = true;

                active_peers_[ep] = peer;
                known_peer_endpoints_.insert(ep);
            }

            peer->worker_thread = std::thread(&TorrentSession::peer_worker_loop, this, peer);
        }
    } catch (...) {
        // Cổng có thể bị chiếm hoặc phiên đã dừng
    }
}

void TorrentSession::accept_utp_stream(std::shared_ptr<pbl4::network::UtpStream> stream) {
    if (!stream || !running_ || paused_) {
        if (stream) stream->close();
        return;
    }

    auto peer = std::make_shared<ActivePeer>();
    peer->connection = std::make_shared<pbl4::network::PeerConnection>(
        std::move(stream), std::min(config_.handshake_timeout, config_.peer_timeout));
    peer->ip = peer->connection->remote_address();
    peer->port = peer->connection->remote_port();
    peer->endpoint_str = peer->ip + ":" + std::to_string(peer->port);
    peer->bitfield.assign(metadata_.pieces.size(), false);
    peer->last_active = std::chrono::steady_clock::now();
    peer->worker_active = true;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!running_ || !inbound_allowed_locked(peer->ip) ||
            active_peers_.find(peer->endpoint_str) != active_peers_.end()) {
            peer->connection->close();
            return;
        }
        active_peers_[peer->endpoint_str] = peer;
    }
    peer->worker_thread = std::thread(&TorrentSession::peer_worker_loop, this, peer);
    std::cout << "[uTP] Accepted peer stream from " << peer->endpoint_str << std::endl;
}

void TorrentSession::connect_candidate_peers() {
    std::vector<std::string> to_connect;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (active_peers_.size() >= config_.max_connections) {
            return;
        }

        while (!candidate_peer_endpoints_.empty() &&
               active_peers_.size() + to_connect.size() < config_.max_connections) {
            std::string candidate = candidate_peer_endpoints_.back();
            candidate_peer_endpoints_.pop_back();
            if (active_peers_.find(candidate) == active_peers_.end()) {
                to_connect.push_back(candidate);
            }
        }
    }

    for (const auto& ep_str : to_connect) {
        const auto colon = ep_str.find(':');
        if (colon == std::string::npos) continue;
        const std::string ip = ep_str.substr(0, colon);
        const std::uint16_t port = static_cast<std::uint16_t>(std::stoi(ep_str.substr(colon + 1)));

        std::cout << "[CONNECT] Attempting TCP connect to " << ep_str << std::endl;
        try {
            auto conn = pbl4::network::PeerConnection::connect_to(
                ip, port, std::chrono::seconds(5));

            std::cout << "[CONNECT] TCP connected to " << ep_str << std::endl;
            auto peer = std::make_shared<ActivePeer>();
            peer->ip = ip;
            peer->port = port;
            peer->outbound = true;  // do mình chủ động mở: không tính vào hạn mức kết nối vào
            peer->endpoint_str = ep_str;
            peer->connection = std::make_shared<pbl4::network::PeerConnection>(std::move(conn));
            peer->bitfield.assign(metadata_.pieces.size(), false);
            peer->last_active = std::chrono::steady_clock::now();
            peer->worker_active = true;

            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                active_peers_[ep_str] = peer;
            }

            peer->worker_thread = std::thread(&TorrentSession::peer_worker_loop, this, peer);
        } catch (const std::exception& ex) {
            std::cout << "[CONNECT] Failed to connect to " << ep_str << ": " << ex.what() << std::endl;
            request_peer_holepunch(ip, port);
        } catch (...) {
            std::cout << "[CONNECT] Failed to connect to " << ep_str << " (unknown error)" << std::endl;
            request_peer_holepunch(ip, port);
        }
    }
}

void TorrentSession::connect_utp_candidates() {
    std::vector<std::string> candidates;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!utp_engine_ || !running_ || paused_ || active_peers_.size() >= config_.max_connections) {
            return;
        }
        while (!utp_peer_candidates_.empty() &&
               active_peers_.size() + candidates.size() < config_.max_connections) {
            std::string endpoint = std::move(utp_peer_candidates_.back());
            utp_peer_candidates_.pop_back();
            if (active_peers_.find(endpoint) == active_peers_.end()) {
                candidates.push_back(std::move(endpoint));
            } else {
                queued_utp_peer_endpoints_.erase(endpoint);
            }
        }
    }

    for (const auto& endpoint : candidates) {
        const auto colon = endpoint.find(':');
        if (colon == std::string::npos) continue;
        const std::string ip = endpoint.substr(0, colon);
        const auto port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
        try {
            const auto address = asio::ip::make_address_v4(ip);
            auto connection = pbl4::network::PeerConnection::connect_utp_to(
                utp_engine_, asio::ip::udp::endpoint(address, port), std::chrono::seconds(8));
            auto peer = std::make_shared<ActivePeer>();
            peer->ip = ip;
            peer->port = port;
            peer->outbound = true;
            peer->endpoint_str = endpoint;
            peer->connection = std::make_shared<pbl4::network::PeerConnection>(std::move(connection));
            peer->bitfield.assign(metadata_.pieces.size(), false);
            peer->last_active = std::chrono::steady_clock::now();
            peer->worker_active = true;

            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                if (!running_ || active_peers_.size() >= config_.max_connections ||
                    active_peers_.find(endpoint) != active_peers_.end()) {
                    peer->connection->close();
                    queued_utp_peer_endpoints_.erase(endpoint);
                    continue;
                }
                active_peers_[endpoint] = peer;
            }
            peer->worker_thread = std::thread(&TorrentSession::peer_worker_loop, this, peer);
            std::cout << "[uTP] Connected to peer " << endpoint << std::endl;
        } catch (const std::exception& ex) {
            std::cout << "[uTP] Connection to " << endpoint << " failed: " << ex.what() << std::endl;
            std::lock_guard<std::mutex> lock(session_mutex_);
            queued_utp_peer_endpoints_.erase(endpoint);
        }
    }
}

void TorrentSession::peer_worker_loop(std::shared_ptr<ActivePeer> peer) {
    if (!peer || !peer->connection) return;

    pbl4::network::PeerHandshake our_hs;
    std::memcpy(our_hs.info_hash.data(), info_hash_raw_.data(), 20);
    std::memcpy(our_hs.peer_id.data(), peer_id_.data(), 20);
    our_hs.set_supports_extension(true);
    our_hs.set_supports_dht(true);
    our_hs.set_supports_fast(true);

    try {
        if (!peer->connection->handshake_completed()) {
            peer->connection->send_handshake(our_hs);
            auto remote_hs = peer->connection->receive_handshake();
            if (std::memcmp(remote_hs.info_hash.data(), info_hash_raw_.data(), 20) != 0) {
                std::cout << "[PEER-WIRE] Handshake info_hash MISMATCH with " << peer->endpoint_str << "! Closing." << std::endl;
                peer->connection->close();
                peer->worker_active = false;
                return;
            }
            peer->remote_peer_id = std::string(
                reinterpret_cast<const char*>(remote_hs.peer_id.data()), 20);
            if (peer->remote_peer_id == peer_id_) {
                // Tự kết nối vào chính mình (vd. địa chỉ của mình quay lại qua tracker).
                peer->connection->close();
                peer->worker_active = false;
                return;
            }
            peer->connection->set_io_timeout(config_.peer_timeout);  // đã bắt tay xong: hết hạn ngắn của kết nối vào
            std::cout << "[PEER-WIRE] Handshake OK with " << peer->endpoint_str
                      << " (peer_id: " << peer->remote_peer_id.substr(0, 8) << "...)" << std::endl;
        } else {
            peer->connection->send_handshake(our_hs);
            std::cout << "[PEER-WIRE] Inbound Handshake reply sent to " << peer->endpoint_str << std::endl;
        }

        peer->fast = peer->connection->remote_handshake().supports_fast();

        // Gửi Bitfield của client cho Peer ngay sau handshake (BEP 6: HaveAll / HaveNone khi cả hai hỗ trợ Fast Extension)
        std::vector<bool> current_bits;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            current_bits = my_bitfield_;
        }
        std::size_t my_has = 0;
        for (bool b : current_bits) if (b) my_has++;
        if (peer->fast && !current_bits.empty() && my_has == current_bits.size()) {
            std::cout << "[PEER-WIRE] Sending HAVE_ALL to " << peer->endpoint_str << std::endl;
            peer->connection->send_have_all();
        } else if (peer->fast && my_has == 0) {
            std::cout << "[PEER-WIRE] Sending HAVE_NONE to " << peer->endpoint_str << std::endl;
            peer->connection->send_have_none();
        } else {
            std::cout << "[PEER-WIRE] Sending bitfield (" << my_has << "/" << current_bits.size()
                      << " pieces) to " << peer->endpoint_str << std::endl;
            peer->connection->send_bitfield(current_bits);
        }

        // BEP 6: cho peer xin một tập piece cố định ngay cả khi họ đang bị choke (khởi động nhanh, không cần chờ unchoke).
        if (peer->fast && !metadata_.pieces.empty()) {
            asio::error_code addr_ec;
            const auto addr = asio::ip::make_address_v4(peer->ip, addr_ec);
            if (!addr_ec) {
                const auto allowed = pbl4::network::compute_allowed_fast_set(
                    addr.to_uint(), info_hash_raw_, static_cast<std::uint32_t>(metadata_.pieces.size()), 10);
                for (std::uint32_t piece_index : allowed) {
                    peer->allowed_fast_for_peer.insert(piece_index);
                    peer->connection->send_allowed_fast(piece_index);
                }
            }
        }

        // Gửi Port message (BEP 5 - ID 9) thông báo DHT port nếu bật DHT
        if (auto* d = dht(); d && peer->connection->is_open()) {
            try {
                peer->connection->send_port(d->port());
            } catch (...) {}
        }

        // Bày tỏ quan tâm nếu đang tải
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (state_ == SessionState::Downloading || state_ == SessionState::Waiting) {
                peer->connection->send_interested();
                peer->am_interested = true;
                std::cout << "[PEER-WIRE] Sent INTERESTED to " << peer->endpoint_str << std::endl;
            }
        }

        // Vòng lặp nhận và xử lý message
        while (running_ && !paused_ && peer->connection->is_open()) {
            bool may_request = false;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                may_request = peer->can_request();
            }
            if (may_request) {
                request_next_blocks(peer);
            }

            // Hết băng thông mà không còn block nào đang chờ: không có gì để nhận, nên chờ token rồi xin lại. Nếu cứ vào
            // receive_message thì ngủ tới peer_timeout (30 giây) mới xin lại, làm tốc độ tải bị giật cục theo chu kỳ đó.
            // Giới hạn 5 giây mỗi lượt để vẫn xử lý được tin nhắn (choke, keep-alive...) của peer.
            if (peer->throttled) {
                const auto spin_start = std::chrono::steady_clock::now();
                while (running_ && !paused_ && peer->connection->is_open() && peer->throttled &&
                       std::chrono::steady_clock::now() - spin_start < std::chrono::seconds(5)) {
                    std::size_t in_flight = 0;
                    {
                        std::lock_guard<std::mutex> lock(session_mutex_);
                        for (const auto& pb : pending_blocks_) {
                            if (pb.peer_endpoint == peer->endpoint_str) ++in_flight;
                        }
                    }
                    if (in_flight != 0) break;  // block đang về: tin nhắn tiếp theo sẽ kích hoạt lần xin kế tiếp
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    request_next_blocks(peer);
                }
            }

            try {
                pbl4::network::PeerMessage msg = peer->connection->receive_message();
                peer->last_active = std::chrono::steady_clock::now();
                handle_peer_message(peer, msg);
            } catch (const pbl4::network::PeerTimeoutError&) {
                if (peer->connection->is_open()) {
                    peer->connection->send_keep_alive();
                }
            }
        }
    } catch (const std::exception& e) {
        // Socket closed or error during peer communication
        std::cout << "[PEER-WIRE] Connection with " << peer->endpoint_str << " ended: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "[PEER-WIRE] Connection with " << peer->endpoint_str << " ended (unknown error)" << std::endl;
    }

    if (peer->connection && peer->connection->is_open()) {
        peer->connection->close();
    }
    peer->worker_active = false;

    // Trả lại các block đang dở mà peer này đang giữ
    std::lock_guard<std::mutex> lock(session_mutex_);
    pending_blocks_.erase(
        std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                       [&](const PendingBlock& pb) { return pb.peer_endpoint == peer->endpoint_str; }),
        pending_blocks_.end());
}

void TorrentSession::handle_peer_message(
    std::shared_ptr<ActivePeer> peer,
    const pbl4::network::PeerMessage& msg) {
    if (msg.keep_alive) {
        return;
    }

    auto msg_id = static_cast<pbl4::network::PeerMessageId>(msg.id);
    switch (msg_id) {
        case pbl4::network::PeerMessageId::Choke: {
            peer->peer_choking = true;
            {
                // Peer huỷ mọi request khi choke; nếu không xoá, các block này kẹt mãi
                // trong pending_blocks_ và pipeline của peer đầy 4/4 nên không tải nữa.
                // BEP 6: với peer Fast, request của piece "allowed fast" vẫn được phục vụ nên giữ lại.
                std::lock_guard<std::mutex> lock(session_mutex_);
                pending_blocks_.erase(
                    std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                                   [&](const PendingBlock& pb) {
                                       return pb.peer_endpoint == peer->endpoint_str &&
                                              !(peer->fast && peer->allowed_fast_to_us.count(pb.piece_index) != 0);
                                   }),
                    pending_blocks_.end());
            }
            std::cout << "[PEER-WIRE] Peer " << peer->endpoint_str << " CHOKED us." << std::endl;
            break;
        }
        case pbl4::network::PeerMessageId::Unchoke: {
            peer->peer_choking = false;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                peer->rejected_pieces.clear();  // trạng thái peer đổi: cho phép thử lại các piece từng bị từ chối
            }
            std::cout << "[PEER-WIRE] Peer " << peer->endpoint_str << " UNCHOKED us! Requesting blocks..." << std::endl;
            request_next_blocks(peer);
            break;
        }
        case pbl4::network::PeerMessageId::Interested: {
            peer->peer_interested = true;
            std::cout << "[PEER-WIRE] Peer " << peer->endpoint_str << " is INTERESTED in our pieces." << std::endl;
            // Unchoke ngay lập tức cho peer nếu mình đang seed hoặc có dữ liệu để bắt đầu truyền tải ngay
            if (peer->am_choking) {
                try {
                    peer->connection->send_unchoke();
                    peer->am_choking = false;
                    std::cout << "[PEER-WIRE] Sent UNCHOKE to " << peer->endpoint_str << " (Allowed to download)" << std::endl;
                } catch (...) {}
            }
            break;
        }
        case pbl4::network::PeerMessageId::NotInterested: {
            peer->peer_interested = false;
            break;
        }
        case pbl4::network::PeerMessageId::Have: {
            auto have = pbl4::network::peer_protocol::parse_have(msg);
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (have.index < peer->bitfield.size()) {
                peer->bitfield[have.index] = true;
            }
            if (have.index < my_bitfield_.size() && !my_bitfield_[have.index] && piece_wanted_[have.index]) {
                if (!peer->am_interested) {
                    peer->connection->send_interested();
                    peer->am_interested = true;
                }
            }
            break;
        }
        case pbl4::network::PeerMessageId::Bitfield: {
            auto bits = pbl4::network::peer_protocol::parse_bitfield(msg, metadata_.pieces.size());
            apply_peer_bitfield(peer, bits);
            break;
        }
        case pbl4::network::PeerMessageId::HaveAll: {
            if (!peer->fast) break;  // chưa thoả thuận Fast Extension: bỏ qua
            apply_peer_bitfield(peer, std::vector<bool>(metadata_.pieces.size(), true));
            break;
        }
        case pbl4::network::PeerMessageId::HaveNone: {
            if (!peer->fast) break;
            apply_peer_bitfield(peer, std::vector<bool>(metadata_.pieces.size(), false));
            break;
        }
        case pbl4::network::PeerMessageId::SuggestPiece: {
            if (!peer->fast) break;
            (void)pbl4::network::peer_protocol::parse_suggest_piece(msg);  // chỉ là gợi ý: piece picker tự quyết
            break;
        }
        case pbl4::network::PeerMessageId::AllowedFast: {
            if (!peer->fast) break;
            const std::uint32_t piece_index = pbl4::network::peer_protocol::parse_allowed_fast(msg);
            bool should_request = false;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                constexpr std::size_t kMaxAllowedFastFromPeer = 64;  // chặn peer gửi vô hạn để phình bộ nhớ
                if (piece_index < metadata_.pieces.size() && peer->allowed_fast_to_us.size() < kMaxAllowedFastFromPeer) {
                    peer->allowed_fast_to_us.insert(piece_index);
                }
                should_request = peer->can_request();
            }
            if (should_request) {
                request_next_blocks(peer);
            }
            break;
        }
        case pbl4::network::PeerMessageId::RejectRequest: {
            if (!peer->fast) break;
            const auto rejected = pbl4::network::peer_protocol::parse_reject_request(msg);
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                // Trả block về để xin nơi khác; piece này không xin lại từ peer này cho tới khi họ unchoke.
                pending_blocks_.erase(
                    std::remove_if(pending_blocks_.begin(), pending_blocks_.end(), [&](const PendingBlock& pb) {
                        return pb.piece_index == rejected.index && pb.begin == rejected.begin &&
                               pb.peer_endpoint == peer->endpoint_str;
                    }),
                    pending_blocks_.end());
                peer->rejected_pieces.insert(rejected.index);
            }
            std::cout << "[PEER-WIRE] Peer " << peer->endpoint_str << " REJECTED request for piece #" << rejected.index
                      << " (begin: " << rejected.begin << ")." << std::endl;
            break;
        }
        case pbl4::network::PeerMessageId::Request: {
            auto req = pbl4::network::peer_protocol::parse_request(msg);
            handle_request_block(peer, req);
            break;
        }
        case pbl4::network::PeerMessageId::Piece: {
            auto piece = pbl4::network::peer_protocol::parse_piece(msg);
            handle_piece_block(peer, piece);
            // Pipeline: ngay sau khi nhận được block, gửi tiếp request block kế tiếp
            // để luôn duy trì 4 request song song → tốc độ tải tối đa
            bool may_request = false;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                may_request = peer->can_request();
            }
            if (may_request) {
                request_next_blocks(peer);
            }
            break;
        }
        case pbl4::network::PeerMessageId::Extended: {
            if (msg.payload.empty()) {
                break;
            }

            const Byte extension_id = msg.payload[0];
            if (extension_id == pbl4::network::extension::kExtendedHandshakeId) {
                try {
                    const auto remote_mapping = pbl4::network::extension::parse_extended_handshake(msg);
                    for (const auto& [name, remote_id] : remote_mapping) {
                        peer->remote_extensions[name] = remote_id;
                    }
                    if (peer->connection->is_open()) {
                        peer->connection->send_message(
                            pbl4::network::extension::make_extended_handshake(
                                std::optional<std::uint32_t>(
                                    static_cast<std::uint32_t>(metadata_.infoRaw.size()))));
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[METADATA] Invalid extended handshake from "
                              << peer->endpoint_str << ": " << e.what() << std::endl;
                }
                break;
            }

            const auto pex_it = peer->remote_extensions.find(pbl4::network::extension::kUtPexName);
            const Byte pex_id = pex_it != peer->remote_extensions.end() ? pex_it->second : pbl4::network::extension::kUtPexId;
            if (extension_id == pex_id) {
                try {
                    const auto pex_msg = pbl4::network::extension::parse_pex_message(msg, pex_id);
                    for (const auto& peer_info : pex_msg.added) {
                        if (!peer_info.ip.empty() && peer_info.port != 0) {
                            add_peer(peer_info.ip, peer_info.port);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[PEX] Failed to handle peer list from "
                              << peer->endpoint_str << ": " << e.what() << std::endl;
                }
                break;
            }

            const auto metadata_it = peer->remote_extensions.find(pbl4::network::extension::kUtMetadataName);
            const Byte metadata_id = metadata_it != peer->remote_extensions.end()
                ? metadata_it->second
                : pbl4::network::extension::kUtMetadataId;
            if (extension_id != metadata_id) {
                break;
            }

            try {
                const auto metadata_msg = pbl4::network::extension::parse_metadata_message(msg, metadata_id);
                if (metadata_msg.type == pbl4::network::extension::MetadataMessage::Type::Request) {
                    const std::size_t total_size = metadata_.infoRaw.size();
                    const std::size_t block_count = (total_size + pbl4::network::extension::kMetadataBlockSize - 1) /
                        pbl4::network::extension::kMetadataBlockSize;
                    if (metadata_msg.piece >= block_count) {
                        peer->connection->send_message(
                            pbl4::network::extension::make_metadata_reject(metadata_id, metadata_msg.piece));
                        break;
                    }

                    const std::size_t offset = static_cast<std::size_t>(metadata_msg.piece) *
                        pbl4::network::extension::kMetadataBlockSize;
                    const std::size_t remaining = std::min<std::size_t>(
                        pbl4::network::extension::kMetadataBlockSize, total_size - offset);
                    const std::string metadata_payload = metadata_.infoRaw;
                    const Bytes block_data(metadata_payload.begin() + static_cast<std::ptrdiff_t>(offset),
                        metadata_payload.begin() + static_cast<std::ptrdiff_t>(offset + remaining));
                    peer->connection->send_message(
                        pbl4::network::extension::make_metadata_data(
                            metadata_id,
                            metadata_msg.piece,
                            static_cast<std::uint32_t>(total_size),
                            block_data));
                }
            } catch (const std::exception& e) {
                std::cerr << "[METADATA] Failed to handle metadata message from "
                          << peer->endpoint_str << ": " << e.what() << std::endl;
            }
            break;
        }
        case pbl4::network::PeerMessageId::Port: {
            const auto remote_dht_port = pbl4::network::peer_protocol::parse_port(msg);
            std::cout << "[DHT] Peer " << peer->endpoint_str << " sent DHT Port: " << remote_dht_port << std::endl;
            if (auto* d = dht(); d && remote_dht_port != 0) {
                d->ping(peer->ip, remote_dht_port);
                // Announce ngay để peer kia biết mình qua DHT
                d->announcePeer(info_hash_raw_, advertised_port_.load());
            }
            break;
        }
        default:
            break;
    }
}

void TorrentSession::handle_request_block(
    std::shared_ptr<ActivePeer> peer,
    const pbl4::network::RequestPayload& req) {
    // BEP 6: với peer Fast, từ chối phải nói rõ bằng Reject Request (peer không phải chờ timeout để biết).
    const auto reject = [&]() {
        if (!peer->fast) return;
        try {
            peer->connection->send_reject_request(req.index, req.begin, req.length);
        } catch (...) {
        }
    };
    // Peer bị choke chỉ được xin piece "allowed fast" (đã gửi cho họ lúc đầu).
    const bool allowed_while_choked = peer->fast && peer->allowed_fast_for_peer.count(req.index) != 0;
    if (peer->am_choking && !allowed_while_choked) {
        std::cout << "[UPLOAD] Rejected request from " << peer->endpoint_str
                  << " because peer is choked by us." << std::endl;
        reject();
        return;
    }

    std::string block_data;
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (req.index < my_bitfield_.size() && my_bitfield_[req.index]) {
            success = piece_manager_->readBlock(req.index, req.begin, req.length, block_data);
        }
    }

    if (success && !block_data.empty()) {
        while (running_ && !consume_upload(block_data.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!running_) {
            return;
        }
        peer->connection->send_piece(
            req.index, req.begin,
            reinterpret_cast<const pbl4::network::Byte*>(block_data.data()),
            block_data.size());

        std::lock_guard<std::mutex> lock(session_mutex_);
        uploaded_bytes_ += block_data.size();
        peer->uploaded_bytes += block_data.size();

        std::cout << "[UPLOAD] Sent piece #" << req.index << " (offset: " << req.begin
                  << ", size: " << block_data.size() << " bytes) to " << peer->endpoint_str
                  << " | Total uploaded: " << uploaded_bytes_ << " bytes" << std::endl;
    } else {
        std::cout << "[UPLOAD] Failed to read piece #" << req.index << " (we might not have it)." << std::endl;
        reject();
    }
}

void TorrentSession::handle_piece_block(
    std::shared_ptr<ActivePeer> peer,
    const pbl4::network::PiecePayload& piece) {
    bool torrent_finished = false;
    PieceCompletedCallback piece_cb;
    std::vector<std::function<void()>> deferred_sends;  // gửi qua socket phải chạy sau khi nhả session_mutex_
    TorrentFinishedCallback finished_cb;
    std::size_t completed_snapshot = 0;
    const std::size_t total_pieces = metadata_.pieces.size();
    std::uint32_t verified_piece = 0;
    bool piece_verified = false;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (piece.index >= total_pieces || my_bitfield_[piece.index]) {
            return;  // ngoài phạm vi, hoặc piece đã verify xong (block trùng)
        }

        // Chỉ nhận block của piece mình đã xin: block tự gửi không được ghi đè dữ liệu.
        auto prog_it = in_progress_pieces_.find(piece.index);
        if (prog_it == in_progress_pieces_.end()) {
            return;
        }
        auto& prog = prog_it->second;

        const std::size_t block_size = pbl4::network::kStandardBlockSize;
        const std::size_t piece_sz = piece_manager_->pieceSize(piece.index);
        const std::size_t block_idx = piece.begin / block_size;
        if (piece.begin % block_size != 0 || block_idx >= prog.received_block_mask.size()) {
            return;
        }
        const std::size_t expected_len = std::min<std::size_t>(block_size, piece_sz - piece.begin);
        if (piece.data.size() != expected_len) {
            return;
        }

        // Ghi block vào PieceManager
        const std::string block_str(
            reinterpret_cast<const char*>(piece.data.data()),
            piece.data.size());

        if (!piece_manager_->writeBlock(piece.index, piece.begin, piece.data.size(), block_str)) {
            return;
        }

        downloaded_bytes_ += piece.data.size();
        peer->downloaded_bytes += piece.data.size();

        // Endgame: block này có thể đã được xin từ peer khác. Gom lại để gửi cancel, khỏi nhận trùng.
        std::vector<PendingBlock> to_cancel;
        for (const auto& pb : pending_blocks_) {
            if (pb.piece_index == piece.index && pb.begin == piece.begin &&
                pb.peer_endpoint != peer->endpoint_str) {
                to_cancel.push_back(pb);
            }
        }

        // Xoá khỏi pending blocks
        pending_blocks_.erase(
            std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                           [&](const PendingBlock& pb) {
                               return pb.piece_index == piece.index && pb.begin == piece.begin;
                           }),
            pending_blocks_.end());

        for (const auto& pb : to_cancel) {
            auto other = active_peers_.find(pb.peer_endpoint);
            if (other != active_peers_.end() && other->second->connection) {
                deferred_sends.push_back([connection = other->second->connection, pb]() {
                    try {
                        connection->send_cancel(pb.piece_index, pb.begin, pb.length);
                        std::cout << "[DOWNLOAD-REQ][ENDGAME] Cancel piece #" << pb.piece_index << " (begin: "
                                  << pb.begin << ") sent to " << pb.peer_endpoint << std::endl;
                    } catch (...) {
                    }
                });
            }
        }

        // Cập nhật tiến độ của piece
        if (!prog.received_block_mask[block_idx]) {
            prog.received_block_mask[block_idx] = true;
            prog.received_blocks++;
        }

        std::cout << "[DOWNLOAD] Received block for piece #" << piece.index << " (offset: " << piece.begin
                  << ", size: " << piece.data.size() << " bytes) from " << peer->endpoint_str
                  << " [" << prog.received_blocks << "/" << prog.total_blocks << " blocks]" << std::endl;

        // Nếu đã nhận đủ mọi block của piece -> verify hash SHA-1
        if (prog.received_blocks >= prog.total_blocks && prog.total_blocks > 0) {
            if (piece_manager_->verifyPiece(piece.index)) {
                my_bitfield_[piece.index] = true;
                completed_pieces_count_++;
                std::cout << "[PIECE-VERIFY] Piece #" << piece.index << " SHA-1 VERIFIED OK! ("
                          << completed_pieces_count_ << "/" << total_pieces << " completed)" << std::endl;

                // Piece ngoài phần được chọn (về muộn sau khi người dùng bỏ file) không tính vào tiến độ.
                const bool piece_was_wanted = piece_wanted_[piece.index];
                if (piece_was_wanted) {
                    if (wanted_remaining_ > 0) {
                        --wanted_remaining_;
                    }
                    if (left_bytes_ >= piece_sz) {
                        left_bytes_ -= piece_sz;
                    } else {
                        left_bytes_ = 0;
                    }
                }

                in_progress_pieces_.erase(piece.index);

                // Lưu ngay piece đã verify vào file .part để tắt app giữa chừng không mất tiến độ.
                const bool persisted = metadata_.isMultiFile()
                    ? piece_manager_->writePieceToFiles(piece.index, save_path_, metadata_.files)
                    : piece_manager_->writePieceToFile(piece.index, part_path_);
                if (!persisted) {
                    std::cout << "[SESSION] Warning: could not persist piece #" << piece.index
                              << " to " << (metadata_.isMultiFile() ? save_path_ : part_path_) << std::endl;
                }

                // Báo cho các peer khác biết mình đã có mảnh này (Have): chụp danh sách kết nối trong khoá, gửi ngoài khoá.
                {
                    std::vector<std::shared_ptr<pbl4::network::PeerConnection>> have_targets;
                    for (auto& [ep, other_peer] : active_peers_) {
                        if (other_peer && other_peer->connection && other_peer->connection->is_open()) {
                            have_targets.push_back(other_peer->connection);
                        }
                    }
                    const std::uint32_t have_index = static_cast<std::uint32_t>(piece.index);
                    deferred_sends.push_back([have_targets, have_index]() {
                        for (const auto& connection : have_targets) {
                            try {
                                connection->send_have(have_index);
                            } catch (...) {
                            }
                        }
                    });
                }

                piece_verified = true;
                verified_piece = piece.index;
                completed_snapshot = completed_pieces_count_;
                piece_cb = piece_cb_;

                // Kiểm tra hoàn tất toàn bộ torrent
                torrent_finished = piece_was_wanted && check_and_finalize_completion();
                if (torrent_finished) {
                    finished_cb = finished_cb_;
                }
            } else {
                // Hash sai: xoá block và tải lại mảnh này
                in_progress_pieces_.erase(piece.index);
            }
        }
    }

    for (const auto& send : deferred_sends) {
        send();
    }

    // Announce tracker và callback phải chạy NGOÀI session_mutex_ (mutex không recursive:
    // process_tracker_announce tự khoá lại, gọi trong lock sẽ deadlock và treo cả Web UI).
    if (piece_verified && piece_cb) {
        piece_cb(verified_piece, completed_snapshot, total_pieces);
    }
    if (torrent_finished) {
        process_tracker_announce("completed");
        if (finished_cb) {
            finished_cb();
        }
    }
}

void TorrentSession::apply_peer_bitfield(const std::shared_ptr<ActivePeer>& peer, const std::vector<bool>& bits) {
    bool should_request = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        peer->bitfield = bits;
        bool interested = false;
        for (std::size_t i = 0; i < bits.size() && i < my_bitfield_.size(); ++i) {
            if (bits[i] && !my_bitfield_[i] && piece_wanted_[i]) {
                interested = true;
                break;
            }
        }
        if (interested && !peer->am_interested) {
            peer->connection->send_interested();
            peer->am_interested = true;
        }
        // Nếu đã được unchoke từ trước (race: unchoke đến trước bitfield) hoặc peer Fast đã cho piece "allowed fast",
        // gửi request ngay sau khi nhả khoá.
        should_request = peer->can_request();
    }
    if (should_request) {
        request_next_blocks(peer);
    }
}

bool TorrentSession::request_next_blocks(std::shared_ptr<ActivePeer> peer) {
    BlockRequest request;
    const bool result = request_next_blocks_locked(peer, request);
    peer->throttled = request.throttled;
    if (request.valid) {
        // Gửi NGOÀI khoá. Gửi lỗi thì bỏ ghi nhận block đang chờ để xin lại từ nơi khác.
        try {
            peer->connection->send_request(request.piece, request.begin, request.length);
        } catch (...) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            pending_blocks_.erase(
                std::remove_if(pending_blocks_.begin(), pending_blocks_.end(), [&](const PendingBlock& pb) {
                    return pb.piece_index == request.piece && pb.begin == request.begin &&
                           pb.peer_endpoint == peer->endpoint_str;
                }),
                pending_blocks_.end());
        }
    }
    return result;
}

bool TorrentSession::request_next_blocks_locked(std::shared_ptr<ActivePeer> peer, BlockRequest& out) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    // Cho phép request trong trạng thái Downloading HOẶC nếu vẫn còn piece cần tải
    if (my_bitfield_.empty()) {
        return false;
    }
    if (state_ != SessionState::Downloading && state_ != SessionState::Checking &&
        state_ != SessionState::Waiting) {
        return false;
    }

    // Giới hạn số request chạy song song mỗi peer (pipeline, do picker quyết định; mặc định 4)
    std::size_t active_peer_requests = 0;
    for (const auto& pb : pending_blocks_) {
        if (pb.peer_endpoint == peer->endpoint_str) {
            active_peer_requests++;
        }
    }
    if (active_peer_requests >= piece_picker_->pipeline_depth()) {
        return false;
    }

    // Peer này phục vụ được piece nào: có piece đó, chưa từng Reject, và (không bị choke hoặc là piece "allowed fast").
    const bool choked = peer->peer_choking;
    const auto peer_can_serve = [&](std::size_t piece_idx) {
        if (piece_idx >= peer->bitfield.size() || !peer->bitfield[piece_idx]) return false;
        const auto index32 = static_cast<std::uint32_t>(piece_idx);
        if (peer->rejected_pieces.count(index32) != 0) return false;
        return !choked || (peer->fast && peer->allowed_fast_to_us.count(index32) != 0);
    };

    // 1. Tìm block chưa tải trong các piece đang dở dang mà peer này có
    for (auto& [piece_idx, prog] : in_progress_pieces_) {
        if (piece_idx < piece_wanted_.size() && !piece_wanted_[piece_idx]) {
            continue;  // người dùng đã bỏ file chứa piece này
        }
        if (peer_can_serve(piece_idx)) {
            for (std::size_t b = 0; b < prog.total_blocks; ++b) {
                if (!prog.received_block_mask[b]) {
                    const std::uint32_t begin = static_cast<std::uint32_t>(b * pbl4::network::kStandardBlockSize);
                    const std::size_t piece_sz = piece_manager_->pieceSize(piece_idx);
                    const std::uint32_t len = static_cast<std::uint32_t>(
                        std::min<std::size_t>(pbl4::network::kStandardBlockSize, piece_sz - begin));

                    // Kiểm tra xem block này đã được request chưa
                    bool already_requested = false;
                    for (const auto& pb : pending_blocks_) {
                        if (pb.piece_index == piece_idx && pb.begin == begin) {
                            already_requested = true;
                            break;
                        }
                    }

                    if (!already_requested) {
                        if (!consume_download(len)) {
                            out.throttled = true;
                            return false;  // hết băng thông tải: thử lại ở lượt sau
                        }
                        out = BlockRequest{true, static_cast<std::uint32_t>(piece_idx), begin, len};
                        pending_blocks_.push_back({piece_idx, begin, len, peer->endpoint_str, std::chrono::steady_clock::now()});
                        return true;
                    }
                }
            }
        }
    }

    // 2. Nếu không có block dở, dùng PiecePicker để chọn mảnh tiếp theo
    std::vector<std::vector<bool>> peers_bits;
    peers_bits.reserve(active_peers_.size());
    for (const auto& [ep, p] : active_peers_) {
        peers_bits.push_back(p->bitfield);
    }

    std::vector<bool> unavailable_or_in_progress = my_bitfield_;
    std::size_t wanted_total = 0;
    for (std::size_t i = 0; i < piece_wanted_.size() && i < unavailable_or_in_progress.size(); ++i) {
        if (piece_wanted_[i]) {
            ++wanted_total;
        } else {
            unavailable_or_in_progress[i] = true;  // không chọn tải: coi như đã có để picker bỏ qua
        }
    }
    for (const auto& [piece_idx, prog] : in_progress_pieces_) {
        if (piece_idx < unavailable_or_in_progress.size()) {
            unavailable_or_in_progress[piece_idx] = true;
        }
    }

    if (choked || !peer->rejected_pieces.empty()) {
        // Chỉ chọn trong những piece peer này thật sự phục vụ được (bị choke -> chỉ piece "allowed fast").
        for (std::size_t i = 0; i < unavailable_or_in_progress.size(); ++i) {
            if (!peer_can_serve(i)) unavailable_or_in_progress[i] = true;
        }
    }

    strategy::PickerContext picker_ctx;
    picker_ctx.total_pieces = wanted_total;
    picker_ctx.completed_pieces = wanted_total >= wanted_remaining_ ? wanted_total - wanted_remaining_ : 0;
    picker_ctx.in_progress_pieces = 0;
    for (const auto& entry : in_progress_pieces_) {
        if (entry.first < piece_wanted_.size() && piece_wanted_[entry.first]) {
            ++picker_ctx.in_progress_pieces;
        }
    }

    const int next_piece = piece_picker_->pick_piece(picker_ctx, unavailable_or_in_progress, peers_bits);
    if (next_piece >= 0 && peer_can_serve(static_cast<std::size_t>(next_piece))) {
        const std::size_t piece_sz = piece_manager_->pieceSize(next_piece);
        const std::size_t block_count = (piece_sz + pbl4::network::kStandardBlockSize - 1) / pbl4::network::kStandardBlockSize;

        const std::uint32_t begin = 0;
        const std::uint32_t len = static_cast<std::uint32_t>(
            std::min<std::size_t>(pbl4::network::kStandardBlockSize, piece_sz));
        if (!consume_download(len)) {
            out.throttled = true;
            return false;  // hết băng thông tải: chưa đánh dấu piece đang tải, thử lại ở lượt sau
        }

        PieceProgress prog;
        prog.total_blocks = block_count;
        prog.received_blocks = 0;
        prog.received_block_mask.assign(block_count, false);
        in_progress_pieces_[next_piece] = prog;

        out = BlockRequest{true, static_cast<std::uint32_t>(next_piece), begin, len};
        pending_blocks_.push_back({static_cast<std::uint32_t>(next_piece), begin, len, peer->endpoint_str, std::chrono::steady_clock::now()});
        std::cout << "[DOWNLOAD-REQ] Sent request for piece #" << next_piece << " (begin: " << begin
                  << ", len: " << len << ") to " << peer->endpoint_str << std::endl;
        return true;
    } else {
        // 3. Endgame: mọi piece còn thiếu đều đã có người xin. Cho phép xin TRÙNG block từ peer
        // này (tối đa endgame_max_copies bản), block nào về trước thắng, bản còn lại bị cancel
        // trong handle_piece_block. Tránh việc cả lượt tải bị kẹt chờ 1 peer chậm ở những block cuối.
        if (piece_picker_->in_endgame(picker_ctx)) {
            const std::size_t max_copies = piece_picker_->endgame_max_copies();
            for (auto& [piece_idx, prog] : in_progress_pieces_) {
                if (!peer_can_serve(piece_idx) ||
                    (piece_idx < piece_wanted_.size() && !piece_wanted_[piece_idx])) {
                    continue;
                }
                for (std::size_t b = 0; b < prog.total_blocks; ++b) {
                    if (prog.received_block_mask[b]) {
                        continue;
                    }
                    const std::uint32_t begin = static_cast<std::uint32_t>(b * pbl4::network::kStandardBlockSize);
                    std::size_t copies = 0;
                    bool requested_by_this_peer = false;
                    std::uint32_t len = 0;
                    for (const auto& pb : pending_blocks_) {
                        if (pb.piece_index == piece_idx && pb.begin == begin) {
                            ++copies;
                            len = pb.length;
                            if (pb.peer_endpoint == peer->endpoint_str) {
                                requested_by_this_peer = true;
                            }
                        }
                    }
                    if (requested_by_this_peer || copies == 0 || copies >= max_copies) {
                        continue;
                    }
                    if (!consume_download(len)) {
                        out.throttled = true;
                        return false;
                    }
                    out = BlockRequest{true, static_cast<std::uint32_t>(piece_idx), begin, len};
                    pending_blocks_.push_back({piece_idx, begin, len, peer->endpoint_str, std::chrono::steady_clock::now()});
                    std::cout << "[DOWNLOAD-REQ][ENDGAME] Duplicate request for piece #" << piece_idx
                              << " (begin: " << begin << ", len: " << len << ") to " << peer->endpoint_str
                              << " | copies: " << (copies + 1) << "/" << max_copies << std::endl;
                    return true;
                }
            }
        }

        static auto last_warn = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - last_warn > std::chrono::seconds(5)) {
            std::cout << "[DOWNLOAD-REQ] Cannot pick piece! picker_result=" << next_piece
                      << " | peer_bitfield_size=" << peer->bitfield.size()
                      << " | our_unavail_pieces=";
            std::size_t unavail = 0;
            for (bool b : unavailable_or_in_progress) if (b) unavail++;
            std::cout << unavail << "/" << unavailable_or_in_progress.size() << std::endl;
            last_warn = now;
        }
    }

    return false;
}

void TorrentSession::broadcast_have(std::uint32_t piece_index) {
    for (auto& [ep, peer] : active_peers_) {
        if (peer && peer->connection && peer->connection->is_open()) {
            try {
                peer->connection->send_have(piece_index);
            } catch (...) {}
        }
    }
}

bool TorrentSession::check_and_finalize_completion() {
    // Gọi khi đang giữ session_mutex_. Trả true nếu vừa hoàn tất; người gọi tự announce
    // "completed" và gọi callback sau khi nhả lock.
    if (all_wanted_done_locked()) {
        try {
            if (metadata_.isMultiFile()) {
                // Piece đã ghi dần vào từng file; chỉ cần bảo đảm đủ file (gồm file dài 0).
                // Chỉ tạo file được chọn; file bị bỏ không được tạo rỗng giả như đã tải xong.
                std::vector<FileEntry> wanted_files;
                for (std::size_t i = 0; i < metadata_.files.size(); ++i) {
                    if (i >= file_wanted_.size() || file_wanted_[i]) {
                        wanted_files.push_back(metadata_.files[i]);
                    }
                }
                if (!PieceManager::ensureFiles(save_path_, wanted_files)) {
                    throw std::runtime_error("cannot create torrent files under " + save_path_.string());
                }
            } else {
                piece_manager_->writeToFile(save_path_);
            }
        } catch (const std::exception& e) {
            error_message_ = std::string("Failed to write completed torrent to disk: ") + e.what();
            set_state(SessionState::Error);
            return false;
        }

        std::error_code rm_ec;
        std::filesystem::remove(part_path_, rm_ec);  // đã có file đầy đủ, bỏ file tạm

        set_state(SessionState::Seeding);
        error_message_.clear();
        return true;
    }
    return false;
}

void TorrentSession::perform_choker_update() {
    // Chỉ quyết định trong khoá; gửi choke/unchoke ngoài khoá (một peer ngừng đọc sẽ chặn lệnh gửi tới 30 giây,
    // và nếu đang giữ session_mutex_ thì cả Web UI treo theo).
    struct ChokeAction {
        std::shared_ptr<ActivePeer> peer;
        bool unchoke = false;
    };
    std::vector<ChokeAction> actions;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (active_peers_.empty() || !choker_) {
            return;
        }

        std::vector<strategy::PeerStatus> statuses;
        statuses.reserve(active_peers_.size());

        for (const auto& [ep, peer] : active_peers_) {
            strategy::PeerStatus ps;
            ps.peer_id = peer->remote_peer_id.empty() ? peer->endpoint_str : peer->remote_peer_id;
            ps.is_interested = peer->peer_interested;
            ps.am_choking_them = peer->am_choking;
            ps.download_rate = peer->download_rate;
            ps.upload_rate = peer->upload_rate;
            statuses.push_back(ps);
        }

        const bool is_seeding = (state_ == SessionState::Seeding);
        const auto unchoked_ids = choker_->update_chokes(statuses, is_seeding);

        for (auto& [ep, peer] : active_peers_) {
            const std::string id = peer->remote_peer_id.empty() ? peer->endpoint_str : peer->remote_peer_id;
            const bool should_unchoke = (std::find(unchoked_ids.begin(), unchoked_ids.end(), id) != unchoked_ids.end());
            if (should_unchoke && peer->am_choking) {
                actions.push_back({peer, true});
            } else if (!should_unchoke && !peer->am_choking) {
                actions.push_back({peer, false});
            }
        }
    }

    for (const auto& action : actions) {
        try {
            if (action.unchoke) {
                action.peer->connection->send_unchoke();
                action.peer->am_choking = false;
            } else {
                action.peer->connection->send_choke();
                action.peer->am_choking = true;
            }
        } catch (...) {
        }
    }
}

void TorrentSession::cleanup_disconnected_peers() {
    std::vector<std::thread> finished_threads;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto it = active_peers_.begin(); it != active_peers_.end(); ) {
            if (!it->second->worker_active || !it->second->connection || !it->second->connection->is_open()) {
                if (it->second->worker_thread.joinable()) {
                    finished_threads.push_back(std::move(it->second->worker_thread));
                }
                // Cho phép kết nối lại: nếu không, peer rớt một lần sẽ bị coi là "đã biết"
                // mãi mãi và tracker/LSD báo lại cũng bị bỏ qua.
                known_peer_endpoints_.erase(it->first);
                it = active_peers_.erase(it);
            } else {
                ++it;
            }
        }

        // Request treo quá lâu (peer im lặng) thì bỏ để block được xin lại từ peer khác.
        const auto now = std::chrono::steady_clock::now();
        pending_blocks_.erase(
            std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                           [&](const PendingBlock& pb) {
                               return now - pb.requested_at > std::chrono::seconds(30);
                           }),
            pending_blocks_.end());
    }
    // Join ngoài lock: luồng worker cần lấy session_mutex_ ở cuối vòng đời.
    // Join (thay vì detach) để không còn luồng nào chạm vào session sau khi nó bị huỷ.
    for (auto& t : finished_threads) {
        if (t.get_id() != std::this_thread::get_id()) {
            t.join();
        }
    }
}

void TorrentSession::update_rates() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rate_calc_time_).count();

    if (elapsed_ms >= 1000) {
        const double elapsed_sec = static_cast<double>(elapsed_ms) / 1000.0;
        const auto dl_delta = downloaded_bytes_ - last_downloaded_snapshot_;
        const auto ul_delta = uploaded_bytes_ - last_uploaded_snapshot_;

        current_download_rate_ = static_cast<double>(dl_delta) / elapsed_sec;
        current_upload_rate_ = static_cast<double>(ul_delta) / elapsed_sec;

        last_downloaded_snapshot_ = downloaded_bytes_;
        last_uploaded_snapshot_ = uploaded_bytes_;
        last_rate_calc_time_ = now;
    }
}

void TorrentSession::process_tracker_announce(const std::string& event) {
    if (!config_.enable_tracker) {
        std::cout << "[TRACKER-ANNOUNCE] Skipping: tracker is disabled by configuration." << std::endl;
        return;
    }
    if (metadata_.announce.empty()) {
        std::cout << "[TRACKER-ANNOUNCE] Skipping: announce URL is empty!" << std::endl;
        return;
    }

    TrackerRequest req;
    req.infoHash = info_hash_raw_;
    req.peerId = peer_id_;
    req.port = advertised_port_.load();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        req.uploaded = uploaded_bytes_;
        req.downloaded = downloaded_bytes_;
        req.left = left_bytes_;
    }
    req.event = event;
    req.numwant = 50;
    req.compact = true;
    req.key = announce_key_hex_;  // giữ chỗ peer_id: người khác biết peer_id cũng không announce/xoá/đổi UDP thay mình

    const std::string url = metadata_.announce;
    std::cout << "[TRACKER-ANNOUNCE] Sending event='" << event
              << "' to " << url
              << " | port=" << req.port
              << " | left=" << req.left << std::endl;

    // Tracker lỗi chỉ đưa session đang tải sang Waiting; tracker phản hồi lại thì phải
    // quay về Downloading, nếu không session kẹt Waiting mãi.
    auto mark_tracker_problem = [this, event](const std::string& message) {
        if (event == "stopped") {
            return;  // đang dừng, tracker trả "failure" cho stopped là bình thường
        }
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!running_) {
            return;
        }
        if (state_ == SessionState::Downloading || state_ == SessionState::Checking) {
            error_message_ = message;
            set_state(SessionState::Waiting);
        }
    };

    ++announce_inflight_;
    std::thread([this, url, req, mark_tracker_problem]() {
        struct InflightGuard {
            std::atomic<int>& counter;
            ~InflightGuard() { --counter; }
        } guard{announce_inflight_};

        try {
            TrackerResponse resp = TrackerClient::announceTracker(url, req);
            if (resp.success) {
                send_udp_registration();
                std::cout << "[TRACKER-ANNOUNCE] Response OK | peers received: "
                          << resp.peers.size() << std::endl;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    if (state_ == SessionState::Waiting && running_) {
                        error_message_.clear();
                        const bool complete = all_wanted_done_locked();
                        set_state(complete ? SessionState::Seeding : SessionState::Downloading);
                    }
                }
                for (const auto& peer : resp.peers) {
                    std::cout << "[TRACKER-ANNOUNCE] Got peer: "
                              << peer.ip << ":" << peer.port << std::endl;
                    add_peer(peer.ip, peer.port);
                }
            } else {
                std::cout << "[TRACKER-ANNOUNCE] Response FAILED: "
                          << resp.failureReason << std::endl;
                mark_tracker_problem(resp.failureReason.empty()
                                         ? "Tracker rejected announce request"
                                         : resp.failureReason);
            }
        } catch (const std::exception& ex) {
            std::cout << "[TRACKER-ANNOUNCE] Exception: " << ex.what() << std::endl;
            mark_tracker_problem(std::string("Tracker temporarily unavailable: ") + ex.what());
        } catch (...) {
            std::cout << "[TRACKER-ANNOUNCE] Unknown exception!" << std::endl;
            mark_tracker_problem("Tracker temporarily unavailable");
        }
    }).detach();
}

}  // namespace p2p::network
