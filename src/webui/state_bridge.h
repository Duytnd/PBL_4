#pragma once

#include "network/torrent_session.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace p2p {

struct TorrentViewInfo {
    std::string name;
    std::string info_hash_hex;
    std::string magnet_uri;
    std::string state_name;
    std::uint64_t total_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t uploaded_bytes = 0;
    double progress_percent = 0.0;
    double download_rate = 0.0;    // bytes/s
    double upload_rate = 0.0;      // bytes/s
    std::size_t completed_pieces = 0;
    std::size_t total_pieces = 0;
    std::size_t connected_peers = 0;
};

class StateBridge {
public:
    static StateBridge& instance() {
        static StateBridge s_instance;
        return s_instance;
    }

    void addSession(std::shared_ptr<TorrentSession> session, const std::string& magnetUri);
    bool removeSession(const std::string& infoHashHex);
    std::shared_ptr<TorrentSession> getSession(const std::string& infoHashHex);

    std::vector<TorrentViewInfo> getAllTorrents();
    std::string toJsonString();

    // Cấp phát cổng kế tiếp cho session mới
    std::uint16_t allocatePort();

private:
    StateBridge() = default;
    ~StateBridge() = default;

    StateBridge(const StateBridge&) = delete;
    StateBridge& operator=(const StateBridge&) = delete;

    mutable std::mutex mutex_;
    struct Entry {
        std::shared_ptr<TorrentSession> session;
        std::string magnet_uri;
    };
    std::unordered_map<std::string, Entry> sessions_;
    std::uint16_t next_port_ = 6881;
};

}  // namespace p2p

