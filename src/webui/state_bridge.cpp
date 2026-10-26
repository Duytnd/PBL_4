#include "webui/state_bridge.h"

#include <iomanip>
#include <sstream>

namespace p2p {

void StateBridge::addSession(std::shared_ptr<TorrentSession> session, const std::string& magnetUri) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string hash = session->info_hash_hex();
    auto it = sessions_.find(hash);
    if (it != sessions_.end()) {
        // Giữ session gốc nếu đã tồn tại để tránh overwrite state và magnet khi add trùng torrent.
        return;
    }
    sessions_[hash] = Entry{std::move(session), magnetUri};
}

bool StateBridge::removeSession(const std::string& infoHashHex) {
    std::shared_ptr<TorrentSession> to_stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(infoHashHex);
        if (it != sessions_.end()) {
            to_stop = it->second.session;
            sessions_.erase(it);
        }
    }
    if (to_stop) {
        to_stop->stop();
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
        info.state_name = stats.state_name;
        info.total_bytes = stats.total_bytes;
        info.downloaded_bytes = stats.downloaded_bytes;
        info.uploaded_bytes = stats.uploaded_bytes;
        info.progress_percent = stats.progress_percent;
        info.download_rate = stats.download_rate;
        info.upload_rate = stats.upload_rate;
        info.completed_pieces = stats.completed_pieces;
        info.total_pieces = stats.total_pieces;
        info.connected_peers = stats.connected_peers;
        info.error_message = stats.error_message;
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
             << "\"error\":\"" << escapeJsonString(t.error_message) << "\""
             << "}";
    }
    json << "]";
    return json.str();
}

}  // namespace p2p

