#include "webui/state_bridge.h"

#include "core/torrent_file.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace p2p {

StateBridge::~StateBridge() {
    {
        std::lock_guard<std::mutex> lock(queue_thread_mutex_);
        queue_stop_ = true;
    }
    queue_cv_.notify_all();
    if (queue_thread_.joinable()) queue_thread_.join();
}

void StateBridge::addSession(std::shared_ptr<TorrentSession> session, const std::string& magnetUri) {
    if (!session) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string hash = session->info_hash_hex();
        if (sessions_.find(hash) != sessions_.end()) {
            // Giữ session gốc nếu đã tồn tại để tránh overwrite state và magnet khi add trùng torrent.
            return;
        }
        sessions_[hash] = Entry{std::move(session), magnetUri, false, next_order_++};
        saveEntryLocked(hash, sessions_[hash]);
    }
    rebalanceQueue();  // torrent mới vượt giới hạn thì xếp hàng ngay
}

void StateBridge::setStateDir(const std::filesystem::path& dir) {
    std::size_t loaded = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_dir_ = dir;
        std::ifstream in(dir / "queue.cfg");
        std::string line;
        std::uint64_t globalDown = 0;
        std::uint64_t globalUp = 0;
        while (std::getline(in, line)) {
            try {
                if (line.rfind("max=", 0) == 0) {
                    loaded = std::min<unsigned long long>(std::stoull(line.substr(4)), 64);
                } else if (line.rfind("global_down=", 0) == 0) {
                    globalDown = std::stoull(line.substr(12));
                } else if (line.rfind("global_up=", 0) == 0) {
                    globalUp = std::stoull(line.substr(10));
                }
            } catch (const std::exception&) {
                // dòng hỏng: giữ mặc định
            }
        }
        max_active_ = loaded;
        global_download_->set_rate(globalDown);
        global_upload_->set_rate(globalUp);
    }
    if (loaded != 0) ensureQueueThread();
}

void StateBridge::saveQueueConfigLocked() const {
    if (state_dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(state_dir_, ec);
    std::ofstream out(state_dir_ / "queue.cfg", std::ios::trunc);
    if (out) {
        out << "max=" << max_active_ << "\n"
            << "global_down=" << global_download_->rate() << "\n"
            << "global_up=" << global_upload_->rate() << "\n";
    }
}

void StateBridge::setGlobalLimits(std::uint64_t downBytesPerSec, std::uint64_t upBytesPerSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    global_download_->set_rate(downBytesPerSec);
    global_upload_->set_rate(upBytesPerSec);
    saveQueueConfigLocked();
}

std::uint64_t StateBridge::globalDownloadLimit() const {
    return global_download_->rate();
}

std::uint64_t StateBridge::globalUploadLimit() const {
    return global_upload_->rate();
}

void StateBridge::applyGlobalLimiters(TorrentSessionConfig& config) const {
    config.global_download_limiter = global_download_;
    config.global_upload_limiter = global_upload_;
}

void StateBridge::ensureQueueThread() {
    std::lock_guard<std::mutex> lock(queue_thread_mutex_);
    if (queue_thread_.joinable() || queue_stop_) return;
    queue_thread_ = std::thread([this]() {
        std::unique_lock<std::mutex> lk(queue_thread_mutex_);
        while (!queue_stop_) {
            queue_cv_.wait_for(lk, std::chrono::seconds(2), [this]() { return queue_stop_; });
            if (queue_stop_) break;
            lk.unlock();
            rebalanceQueue();  // bắt các torrent vừa tải xong để nhường chỗ cho torrent đang chờ
            lk.lock();
        }
    });
}

void StateBridge::setMaxActiveDownloads(std::size_t max) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        max_active_ = std::min<std::size_t>(max, 64);
        saveQueueConfigLocked();
    }
    if (max != 0) ensureQueueThread();
    rebalanceQueue();
}

std::size_t StateBridge::maxActiveDownloads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_active_;
}

bool StateBridge::isQueued(const std::string& infoHashHex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(infoHashHex);
    return it != sessions_.end() && it->second.queued;
}

void StateBridge::rebalanceQueue() {
    std::lock_guard<std::mutex> serial(rebalance_mutex_);
    std::vector<std::pair<std::shared_ptr<TorrentSession>, bool>> actions;  // true = resume, false = pause
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> active, waiting;
        for (auto& [hash, entry] : sessions_) {
            if (!entry.session) continue;
            if (max_active_ == 0) {
                if (entry.queued) {  // tắt giới hạn: thả hết hàng đợi
                    entry.queued = false;
                    actions.emplace_back(entry.session, true);
                }
                continue;
            }
            if (entry.session->is_complete()) {
                entry.queued = false;  // đã xong: không còn nằm trong hàng đợi (đang seed không tính vào giới hạn)
                continue;
            }
            if (!entry.session->is_paused()) {
                active.push_back(hash);
            } else if (entry.queued) {
                waiting.push_back(hash);
            }
        }
        if (max_active_ != 0) {
            const auto byOrder = [this](const std::string& a, const std::string& b) {
                return sessions_[a].order < sessions_[b].order;
            };
            std::sort(active.begin(), active.end(), byOrder);
            std::sort(waiting.begin(), waiting.end(), byOrder);
            for (std::size_t i = max_active_; i < active.size(); ++i) {
                Entry& entry = sessions_[active[i]];
                entry.queued = true;
                actions.emplace_back(entry.session, false);
            }
            for (std::size_t i = 0; i < waiting.size() && active.size() + i < max_active_; ++i) {
                Entry& entry = sessions_[waiting[i]];
                entry.queued = false;
                actions.emplace_back(entry.session, true);
            }
        }
    }
    for (const auto& [session, resume] : actions) {
        if (resume) session->resume(); else session->pause();
    }
}

bool StateBridge::userPause(const std::string& infoHashHex) {
    std::shared_ptr<TorrentSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(infoHashHex);
        if (it == sessions_.end() || !it->second.session) return false;
        it->second.queued = false;  // người dùng đã chủ động dừng: không tự chạy lại khi có chỗ
        session = it->second.session;
    }
    session->pause();
    persistState(infoHashHex);
    rebalanceQueue();  // nhường chỗ cho torrent đang chờ
    return true;
}

bool StateBridge::userResume(const std::string& infoHashHex) {
    std::shared_ptr<TorrentSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(infoHashHex);
        if (it == sessions_.end() || !it->second.session) return false;
        it->second.queued = false;
        if (max_active_ != 0) {
            it->second.order = --front_order_;  // đẩy lên đầu hàng
        }
        session = it->second.session;
    }
    session->resume();
    persistState(infoHashHex);
    rebalanceQueue();  // vượt giới hạn thì torrent mới nhất đang chạy phải nhường chỗ
    return true;
}

// Gọi khi đang giữ mutex_. Lỗi ghi đĩa chỉ làm mất khả năng khôi phục, không làm hỏng torrent đang chạy.
void StateBridge::saveEntryLocked(const std::string& hash, const Entry& entry) const {
    if (state_dir_.empty() || !entry.session) return;
    std::error_code ec;
    std::filesystem::create_directories(state_dir_, ec);

    const TorrentFileMetadata& meta = entry.session->metadata();
    std::string torrent = "d";
    if (!meta.announce.empty()) {
        torrent += "8:announce" + std::to_string(meta.announce.size()) + ":" + meta.announce;
    }
    torrent += "4:info" + meta.infoRaw + "e";
    {
        std::ofstream out(state_dir_ / (hash + ".torrent"), std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(torrent.data(), static_cast<std::streamsize>(torrent.size()));
    }
    std::ofstream out(state_dir_ / (hash + ".meta"), std::ios::trunc);
    if (!out) return;
    out << "magnet=" << entry.magnet_uri << "\n"
        << "paused=" << (entry.session->is_paused() && !entry.queued ? 1 : 0) << "\n"  // dừng do hàng đợi: lần sau tự xếp lại
        << "down=" << entry.session->download_limit() << "\n"
        << "up=" << entry.session->upload_limit() << "\n";
    const std::vector<bool> selection = entry.session->file_selection();
    std::string skipped;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        if (!selection[i]) {
            skipped += (skipped.empty() ? "" : ",") + std::to_string(i);
        }
    }
    if (!skipped.empty()) {
        out << "skip=" << skipped << "\n";
    }
}

void StateBridge::persistState(const std::string& infoHashHex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(infoHashHex);
    if (it != sessions_.end()) {
        saveEntryLocked(infoHashHex, it->second);
    }
}

std::vector<SavedTorrent> StateBridge::loadSavedTorrents() const {
    std::filesystem::path dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = state_dir_;
    }
    std::vector<SavedTorrent> result;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".torrent") continue;
        try {
            SavedTorrent saved;
            saved.metadata = TorrentFile::parseFile(entry.path().string());
            std::ifstream meta(std::filesystem::path(entry.path()).replace_extension(".meta"));
            std::string line;
            while (std::getline(meta, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                try {
                    if (key == "magnet") saved.magnet_uri = value;
                    else if (key == "paused") saved.paused = value == "1";
                    else if (key == "down") saved.download_limit = std::stoull(value);
                    else if (key == "up") saved.upload_limit = std::stoull(value);
                    else if (key == "skip") {
                        std::stringstream list(value);
                        std::string item;
                        while (std::getline(list, item, ',')) {
                            if (!item.empty()) saved.skipped_files.push_back(static_cast<std::size_t>(std::stoull(item)));
                        }
                    }
                } catch (const std::exception&) {
                    // giá trị hỏng: giữ mặc định
                }
            }
            result.push_back(std::move(saved));
        } catch (const std::exception& e) {
            std::cerr << "[STATE] Skipping unreadable saved torrent " << entry.path() << ": " << e.what() << std::endl;
        }
    }
    return result;
}

bool StateBridge::removeSession(const std::string& infoHashHex) {
    std::shared_ptr<TorrentSession> to_stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(infoHashHex);
        if (it != sessions_.end()) {
            to_stop = it->second.session;
            sessions_.erase(it);
            if (!state_dir_.empty()) {
                std::error_code ec;
                std::filesystem::remove(state_dir_ / (infoHashHex + ".torrent"), ec);
                std::filesystem::remove(state_dir_ / (infoHashHex + ".meta"), ec);
            }
        }
    }
    if (to_stop) {
        to_stop->stop();
        rebalanceQueue();  // chỗ trống cho torrent đang chờ
        return true;
    }
    return false;
}

std::shared_ptr<TorrentSession> StateBridge::getSession(const std::string& infoHashHex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(infoHashHex);
    if (it != sessions_.end()) {
        return it->second.session;
    }
    return nullptr;
}

std::vector<TorrentViewInfo> StateBridge::getAllTorrents() {
    std::vector<TorrentViewInfo> list;
    std::lock_guard<std::mutex> lock(mutex_);
    list.reserve(sessions_.size());

    for (const auto& [hash, entry] : sessions_) {
        if (!entry.session) continue;
        auto stats = entry.session->stats();
        TorrentViewInfo info;
        info.name = entry.session->metadata().name;
        info.info_hash_hex = hash;
        info.magnet_uri = entry.magnet_uri;
        info.queued = entry.queued;
        info.state_name = entry.queued ? "Queued" : stats.state_name;
        info.total_bytes = stats.total_bytes;
        info.downloaded_bytes = stats.downloaded_bytes;
        info.uploaded_bytes = stats.uploaded_bytes;
        info.progress_percent = stats.progress_percent;
        info.download_rate = stats.download_rate;
        info.upload_rate = stats.upload_rate;
        info.completed_pieces = stats.completed_pieces;
        info.total_pieces = stats.total_pieces;
        info.connected_peers = stats.connected_peers;
        info.dht_nodes = shared_dht_ ? shared_dht_->routingTable().size() : stats.dht_nodes_count;
        info.error_message = stats.error_message;
        info.paused = entry.session->is_paused() && !entry.queued;
        info.download_limit = entry.session->download_limit();
        info.upload_limit = entry.session->upload_limit();
        list.push_back(std::move(info));
    }
    return list;
}

namespace {

// Thử bind thật (không reuse) để biết cổng có đang bị process khác giữ không.
bool isPortFree(std::uint16_t port) {
    try {
        asio::io_context ioc;
        asio::ip::tcp::acceptor probe(ioc);
        const asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
        probe.open(ep.protocol());
        probe.bind(ep);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

std::uint16_t StateBridge::allocatePort() {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr std::uint16_t kFirst = 6881;
    constexpr std::uint16_t kLast = 6950;
    for (int attempt = 0; attempt <= kLast - kFirst; ++attempt) {
        const std::uint16_t port = next_port_;
        next_port_ = (next_port_ >= kLast) ? kFirst : static_cast<std::uint16_t>(next_port_ + 1);
        if (isPortFree(port)) {
            return port;
        }
    }
    // Cả dải 6881-6950 đều bận: trả về cổng kế tiếp, start() sẽ báo lỗi bind rõ ràng.
    return next_port_;
}

std::shared_ptr<KademliaNode> StateBridge::sharedDht() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shared_dht_;
}

std::shared_ptr<KademliaNode> StateBridge::getOrCreateSharedDht(
    std::uint16_t preferredPort,
    const std::string& bootstrapHost,
    std::uint16_t bootstrapPort) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shared_dht_) {
        if (!bootstrapHost.empty() && bootstrapPort != 0) {
            std::thread([dht = shared_dht_, host = bootstrapHost, port = bootstrapPort]() {
                dht->bootstrap(host, port);
            }).detach();
        }
        return shared_dht_;
    }

    std::uint16_t port = preferredPort != 0 ? preferredPort : 6881;
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto node = std::make_shared<KademliaNode>(NodeId::random(), port);
        if (node->start()) {
            std::cout << "[DHT] Shared Kademlia node started on UDP port " << node->port() << std::endl;
            shared_dht_ = std::move(node);
            break;
        }
        port++;
    }

    if (shared_dht_ && !bootstrapHost.empty() && bootstrapPort != 0) {
        std::thread([dht = shared_dht_, host = bootstrapHost, port = bootstrapPort]() {
            dht->bootstrap(host, port);
        }).detach();
    }

    return shared_dht_;
}

static std::string escapeJsonString(const std::string& input) {
    std::ostringstream ss;
    for (char c : input) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    ss << c;
                }
        }
    }
    return ss.str();
}

std::string StateBridge::toJsonString() {
    const auto list = getAllTorrents();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& t = list[i];
        if (i > 0) json << ",";
        json << "{"
             << "\"name\":\"" << escapeJsonString(t.name) << "\","
             << "\"info_hash\":\"" << t.info_hash_hex << "\","
             << "\"magnet_uri\":\"" << escapeJsonString(t.magnet_uri) << "\","
             << "\"state\":\"" << escapeJsonString(t.state_name) << "\","
             << "\"total_bytes\":" << t.total_bytes << ","
             << "\"downloaded_bytes\":" << t.downloaded_bytes << ","
             << "\"uploaded_bytes\":" << t.uploaded_bytes << ","
             << "\"progress_percent\":" << std::fixed << std::setprecision(1) << t.progress_percent << ","
             << "\"download_rate\":" << static_cast<uint64_t>(t.download_rate) << ","
             << "\"upload_rate\":" << static_cast<uint64_t>(t.upload_rate) << ","
             << "\"completed_pieces\":" << t.completed_pieces << ","
             << "\"total_pieces\":" << t.total_pieces << ","
             << "\"connected_peers\":" << t.connected_peers << ","
             << "\"dht_nodes\":" << t.dht_nodes << ","
             << "\"paused\":" << (t.paused ? "true" : "false") << ","
             << "\"queued\":" << (t.queued ? "true" : "false") << ","
             << "\"download_limit\":" << t.download_limit << ","
             << "\"upload_limit\":" << t.upload_limit << ","
             << "\"error\":\"" << escapeJsonString(t.error_message) << "\""
             << "}";
    }
    json << "]";
    return json.str();
}

}  // namespace p2p

