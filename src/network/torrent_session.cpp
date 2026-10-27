#include "network/torrent_session.h"

#include "core/hash_utils.h"
#include "network/lsd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

TorrentSession::TorrentSession(
    const TorrentFileMetadata& metadata,
    const TorrentSessionConfig& config)
    : metadata_(metadata),
      config_(config) {
    if (config_.client_peer_id.empty()) {
        peer_id_ = generate_azureus_peer_id();
    } else {
        peer_id_ = config_.client_peer_id;
        peer_id_.resize(20, '0');
    }

    info_hash_raw_ = p2p::sha1Digest(metadata_.infoRaw);
    info_hash_hex_ = p2p::sha1Hex(metadata_.infoRaw);

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

    piece_manager_ = std::make_unique<PieceManager>(
        piece_count,
        static_cast<std::size_t>(metadata_.pieceLength),
        static_cast<std::size_t>(total_bytes_),
        metadata_.pieces);

    // Mặc định sử dụng chiến lược Rarest-First và Tit-For-Tat
    piece_picker_ = std::make_unique<strategy::RarestFirstPicker>();
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

    // File đầy đủ ưu tiên; nếu chưa có thì resume từ file tạm .part.
    const std::filesystem::path& source =
        std::filesystem::exists(save_path_) ? save_path_ : part_path_;
    if (std::filesystem::exists(source)) {
        if (piece_manager_->loadFromFile(source)) {
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

    if (!metadata_.pieces.empty() && completed_pieces_count_ == metadata_.pieces.size()) {
        set_state(SessionState::Seeding);
        left_bytes_ = 0;
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

    running_ = true;
    paused_ = false;

    if (completed_pieces_count_ == metadata_.pieces.size() && !metadata_.pieces.empty()) {
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
            const std::uint16_t dport = config_.dht_port != 0 ? config_.dht_port : config_.listen_port;
            dht_ = std::make_unique<KademliaNode>(NodeId::random(), dport);
            if (dht_->start()) {
                std::cout << "[DHT] Kademlia node started on UDP port " << dht_->port() << std::endl;

                dht_init_thread_ = std::thread([this]() {
                    if (!running_) {
                        return;
                    }

                    if (!config_.dht_bootstrap_host.empty() && config_.dht_bootstrap_port != 0) {
                        dht_->bootstrap(config_.dht_bootstrap_host, config_.dht_bootstrap_port);
                    }

                    if (!running_) {
                        return;
                    }

                    const auto dht_peers = dht_->getPeers(info_hash_raw_);
                    for (const auto& peer : dht_peers) {
                        if (running_) {
                            add_peer(peer.ip, peer.port);
                        }
                    }

                    if (running_) {
                        dht_->announcePeer(info_hash_raw_, config_.listen_port);
                    }
                });
            } else {
                std::cout << "[DHT] Could not bind Kademlia node on port " << dport << ", continuing without DHT" << std::endl;
                dht_.reset();
            }
        } catch (...) {
            dht_.reset();
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

    if (dht_init_thread_.joinable()) {
        dht_init_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (dht_) {
            dht_->stop();
        }
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

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        active_peers_.clear();
        in_progress_pieces_.clear();
        pending_blocks_.clear();
        lsd_.reset();
        dht_.reset();
    }
}

void TorrentSession::pause() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (running_ && !paused_) {
        paused_ = true;
        set_state(SessionState::Paused);
    }
}

void TorrentSession::resume() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (running_ && paused_) {
        paused_ = false;
        if (completed_pieces_count_ == metadata_.pieces.size() && !metadata_.pieces.empty()) {
            set_state(SessionState::Seeding);
        } else {
            set_state(SessionState::Downloading);
        }
    }
}

bool TorrentSession::is_running() const noexcept {
    return running_;
}

bool TorrentSession::is_complete() const noexcept {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return !metadata_.pieces.empty() && completed_pieces_count_ == metadata_.pieces.size();
}

SessionState TorrentSession::state() const noexcept {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return state_;
}

void TorrentSession::add_peer(const std::string& ip, std::uint16_t port) {
    if (ip.empty() || port == 0) return;
    const std::string ep = ip + ":" + std::to_string(port);

    bool is_new = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (known_peer_endpoints_.insert(ep).second) {
            candidate_peer_endpoints_.push_back(ep);
            is_new = true;
        }
    }
    std::cout << "[ADD-PEER] " << ep
              << (is_new ? " -> added to candidates" : " -> already known, skipped")
              << " | running=" << running_ << " paused=" << paused_ << std::endl;
    if (running_ && !paused_) {
        connect_candidate_peers();
    }
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
    s.total_pieces = metadata_.pieces.size();
    s.completed_pieces = completed_pieces_count_;
    s.connected_peers = active_peers_.size();
    s.total_peers_known = known_peer_endpoints_.size();
    s.dht_nodes_count = dht_ ? dht_->routingTable().size() : 0;
    s.download_rate = current_download_rate_;
    s.upload_rate = current_upload_rate_;
    s.error_message = error_message_;

    // Nếu đang Seeding → luôn 100%
    if (state_ == SessionState::Seeding) {
        s.progress_percent = 100.0;
    } else if (total_bytes_ > 0 && s.total_pieces > 0) {
        // Byte của piece đã verify + các block đã nhận của piece đang dở, để thanh tiến độ
        // chạy liên tục thay vì nhảy từng piece (piece 256KB trở lên trông như đứng yên).
        std::uint64_t partial = 0;
        for (const auto& [idx, prog] : in_progress_pieces_) {
            partial += static_cast<std::uint64_t>(prog.received_blocks) * pbl4::network::kStandardBlockSize;
        }
        const std::uint64_t verified = total_bytes_ > left_bytes_ ? total_bytes_ - left_bytes_ : 0;
        const std::uint64_t done = std::min<std::uint64_t>(verified + partial, total_bytes_);
        s.progress_percent = std::clamp(
            (static_cast<double>(done) / static_cast<double>(total_bytes_)) * 100.0, 0.0, 100.0);
        if (s.completed_pieces < s.total_pieces) {
            s.progress_percent = std::min(s.progress_percent, 99.9);  // 100% chỉ khi đã verify hết
        }
    } else if (total_bytes_ > 0) {
        const double downloaded = static_cast<double>(total_bytes_ - left_bytes_);
        s.progress_percent = std::clamp((downloaded / static_cast<double>(total_bytes_)) * 100.0, 0.0, 100.0);
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

void TorrentSession::orchestration_loop() {
    if (config_.enable_tracker) {
        process_tracker_announce("started");
    }

    auto last_choker_time = std::chrono::steady_clock::now();
    auto last_tracker_time = std::chrono::steady_clock::now();
    auto last_lsd_time = std::chrono::steady_clock::now();
    auto last_dht_time = std::chrono::steady_clock::now();

    while (running_) {
        if (!paused_) {
            const auto now = std::chrono::steady_clock::now();

            // 1. Cập nhật tốc độ & bảo trì kết nối mỗi giây
            update_rates();
            cleanup_disconnected_peers();
            connect_candidate_peers();

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

            // 4. Announce định kỳ LSD (mỗi 60 giây)
            if (config_.enable_lsd && lsd_ && now - last_lsd_time >= std::chrono::seconds(60)) {
                lsd_->announce(info_hash_raw_);
                last_lsd_time = now;
            }

            // 5. Announce và truy vấn định kỳ DHT (mỗi 60 giây)
            if (config_.enable_dht && dht_ && now - last_dht_time >= std::chrono::seconds(60)) {
                const auto dht_peers = dht_->getPeers(info_hash_raw_);
                for (const auto& p : dht_peers) {
                    add_peer(p.ip, p.port);
                }
                dht_->announcePeer(info_hash_raw_, config_.listen_port);
                last_dht_time = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
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
                if (active_peers_.size() >= config_.max_connections) {
                    socket.close(ec);
                    continue;
                }
                peer = std::make_shared<ActivePeer>();
                peer->ip = remote_ip;
                peer->port = remote_port;
                peer->endpoint_str = ep;
                peer->connection = std::make_shared<pbl4::network::PeerConnection>(
                    std::move(socket), config_.peer_timeout);
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
            std::lock_guard<std::mutex> lock(session_mutex_);
            known_peer_endpoints_.erase(ep_str);
        } catch (...) {
            std::cout << "[CONNECT] Failed to connect to " << ep_str << " (unknown error)" << std::endl;
            std::lock_guard<std::mutex> lock(session_mutex_);
            known_peer_endpoints_.erase(ep_str);
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
            std::cout << "[PEER-WIRE] Handshake OK with " << peer->endpoint_str
                      << " (peer_id: " << peer->remote_peer_id.substr(0, 8) << "...)" << std::endl;
        } else {
            peer->connection->send_handshake(our_hs);
            std::cout << "[PEER-WIRE] Inbound Handshake reply sent to " << peer->endpoint_str << std::endl;
        }

        // Gửi Bitfield của client cho Peer ngay sau handshake
        std::vector<bool> current_bits;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            current_bits = my_bitfield_;
        }
        std::size_t my_has = 0;
        for (bool b : current_bits) if (b) my_has++;
        std::cout << "[PEER-WIRE] Sending bitfield (" << my_has << "/" << current_bits.size()
                  << " pieces) to " << peer->endpoint_str << std::endl;
        peer->connection->send_bitfield(current_bits);

        // Gửi Port message (BEP 5 - ID 9) thông báo DHT port nếu bật DHT
        if (dht_ && peer->connection->is_open()) {
            try {
                peer->connection->send_port(dht_->port());
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
            if (!peer->peer_choking && peer->am_interested) {
                request_next_blocks(peer);
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
    } catch (...) {
        // Socket closed or error during peer communication
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
                std::lock_guard<std::mutex> lock(session_mutex_);
                pending_blocks_.erase(
                    std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                                   [&](const PendingBlock& pb) { return pb.peer_endpoint == peer->endpoint_str; }),
                    pending_blocks_.end());
            }
            std::cout << "[PEER-WIRE] Peer " << peer->endpoint_str << " CHOKED us." << std::endl;
            break;
        }
        case pbl4::network::PeerMessageId::Unchoke: {
            peer->peer_choking = false;
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
            if (have.index < my_bitfield_.size() && !my_bitfield_[have.index]) {
                if (!peer->am_interested) {
                    peer->connection->send_interested();
                    peer->am_interested = true;
                }
            }
            break;
        }
        case pbl4::network::PeerMessageId::Bitfield: {
            auto bits = pbl4::network::peer_protocol::parse_bitfield(msg, metadata_.pieces.size());

            bool should_request = false;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                peer->bitfield = bits;
                bool interested = false;
                for (std::size_t i = 0; i < bits.size() && i < my_bitfield_.size(); ++i) {
                    if (bits[i] && !my_bitfield_[i]) {
                        interested = true;
                        break;
                    }
                }
                if (interested && !peer->am_interested) {
                    peer->connection->send_interested();
                    peer->am_interested = true;
                }
                // Nếu đã được unchoke từ trước (race: unchoke đến trước bitfield),
                // đánh dấu để gửi request ngay sau khi giải phóng lock
                should_request = (!peer->peer_choking && peer->am_interested);
            }
            if (should_request) {
                request_next_blocks(peer);
            }
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
            if (!peer->peer_choking && peer->am_interested) {
                request_next_blocks(peer);
            }
            break;
        }
        case pbl4::network::PeerMessageId::Port: {
            const auto remote_dht_port = pbl4::network::peer_protocol::parse_port(msg);
            std::cout << "[DHT] Peer " << peer->endpoint_str << " sent DHT Port: " << remote_dht_port << std::endl;
            if (dht_ && remote_dht_port != 0) {
                dht_->ping(peer->ip, remote_dht_port);
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
    if (peer->am_choking) {
        std::cout << "[UPLOAD] Rejected request from " << peer->endpoint_str
                  << " because peer is choked by us." << std::endl;
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
    }
}

void TorrentSession::handle_piece_block(
    std::shared_ptr<ActivePeer> peer,
    const pbl4::network::PiecePayload& piece) {
    bool torrent_finished = false;
    PieceCompletedCallback piece_cb;
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

        // Xoá khỏi pending blocks
        pending_blocks_.erase(
            std::remove_if(pending_blocks_.begin(), pending_blocks_.end(),
                           [&](const PendingBlock& pb) {
                               return pb.piece_index == piece.index && pb.begin == piece.begin;
                           }),
            pending_blocks_.end());

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

                if (left_bytes_ >= piece_sz) {
                    left_bytes_ -= piece_sz;
                } else {
                    left_bytes_ = 0;
                }

                in_progress_pieces_.erase(piece.index);

                // Lưu ngay piece đã verify vào file .part để tắt app giữa chừng không mất tiến độ.
                if (!piece_manager_->writePieceToFile(piece.index, part_path_)) {
                    std::cout << "[SESSION] Warning: could not persist piece #" << piece.index
                              << " to " << part_path_ << std::endl;
                }

                // Báo cho các peer khác biết mình đã có mảnh này (Have)
                broadcast_have(piece.index);

                piece_verified = true;
                verified_piece = piece.index;
                completed_snapshot = completed_pieces_count_;
                piece_cb = piece_cb_;

                // Kiểm tra hoàn tất toàn bộ torrent
                torrent_finished = check_and_finalize_completion();
                if (torrent_finished) {
                    finished_cb = finished_cb_;
                }
            } else {
                // Hash sai: xoá block và tải lại mảnh này
                in_progress_pieces_.erase(piece.index);
            }
        }
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

bool TorrentSession::request_next_blocks(std::shared_ptr<ActivePeer> peer) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    // Cho phép request trong trạng thái Downloading HOẶC nếu vẫn còn piece cần tải
    if (my_bitfield_.empty()) {
        return false;
    }
    if (state_ != SessionState::Downloading && state_ != SessionState::Checking &&
        state_ != SessionState::Waiting) {
        return false;
    }

    // Giới hạn số request chạy song song mỗi peer (Pipeline: 4)
    std::size_t active_peer_requests = 0;
    for (const auto& pb : pending_blocks_) {
        if (pb.peer_endpoint == peer->endpoint_str) {
            active_peer_requests++;
        }
    }
    if (active_peer_requests >= 4) {
        return false;
    }

    // 1. Tìm block chưa tải trong các piece đang dở dang mà peer này có
    for (auto& [piece_idx, prog] : in_progress_pieces_) {
        if (piece_idx < peer->bitfield.size() && peer->bitfield[piece_idx]) {
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
                        peer->connection->send_request(piece_idx, begin, len);
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
    for (const auto& [piece_idx, prog] : in_progress_pieces_) {
        if (piece_idx < unavailable_or_in_progress.size()) {
            unavailable_or_in_progress[piece_idx] = true;
        }
    }

    const int next_piece = piece_picker_->pick_next_piece(unavailable_or_in_progress, peers_bits);
    if (next_piece >= 0 && static_cast<std::size_t>(next_piece) < peer->bitfield.size() && peer->bitfield[next_piece]) {
        const std::size_t piece_sz = piece_manager_->pieceSize(next_piece);
        const std::size_t block_count = (piece_sz + pbl4::network::kStandardBlockSize - 1) / pbl4::network::kStandardBlockSize;

        PieceProgress prog;
        prog.total_blocks = block_count;
        prog.received_blocks = 0;
        prog.received_block_mask.assign(block_count, false);
        in_progress_pieces_[next_piece] = prog;

        const std::uint32_t begin = 0;
        const std::uint32_t len = static_cast<std::uint32_t>(
            std::min<std::size_t>(pbl4::network::kStandardBlockSize, piece_sz));

        peer->connection->send_request(next_piece, begin, len);
        pending_blocks_.push_back({static_cast<std::uint32_t>(next_piece), begin, len, peer->endpoint_str, std::chrono::steady_clock::now()});
        std::cout << "[DOWNLOAD-REQ] Sent request for piece #" << next_piece << " (begin: " << begin
                  << ", len: " << len << ") to " << peer->endpoint_str << std::endl;
        return true;
    } else {
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
    if (completed_pieces_count_ >= metadata_.pieces.size() && !metadata_.pieces.empty()) {
        try {
            piece_manager_->writeToFile(save_path_);
        } catch (const std::exception& e) {
            error_message_ = std::string("Failed to write completed torrent to disk: ") + e.what();
            set_state(SessionState::Error);
            return false;
        }

        std::error_code rm_ec;
        std::filesystem::remove(part_path_, rm_ec);  // đã có file đầy đủ, bỏ file tạm

        set_state(SessionState::Seeding);
        error_message_.clear();
        left_bytes_ = 0;
        return true;
    }
    return false;
}

void TorrentSession::perform_choker_update() {
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
            try {
                peer->connection->send_unchoke();
                peer->am_choking = false;
            } catch (...) {}
        } else if (!should_unchoke && !peer->am_choking) {
            try {
                peer->connection->send_choke();
                peer->am_choking = true;
            } catch (...) {}
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
    req.port = config_.listen_port;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        req.uploaded = uploaded_bytes_;
        req.downloaded = downloaded_bytes_;
        req.left = left_bytes_;
    }
    req.event = event;
    req.numwant = 50;
    req.compact = true;

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
            TrackerResponse resp = TrackerClient::announceHttp(url, req);
            if (resp.success) {
                std::cout << "[TRACKER-ANNOUNCE] Response OK | peers received: "
                          << resp.peers.size() << std::endl;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    if (state_ == SessionState::Waiting && running_) {
                        error_message_.clear();
                        const bool complete =
                            !metadata_.pieces.empty() && completed_pieces_count_ == metadata_.pieces.size();
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

