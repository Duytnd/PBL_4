#pragma once

#include "network/tracker_protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace p2p {

struct PeerRegistration {
    std::string peerId;
    std::string ip;
    std::uint16_t port = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t downloaded = 0;
    std::uint64_t left = 0;
};

// Lưu danh sách peer theo info_hash, tự xoá peer đã hết hạn.
// Tách khỏi TrackerServer để test được riêng và vì đây là phần dữ liệu
// thuần (mục 6 của kế hoạch: tracker/peer_registry).
class PeerRegistry {
public:
    using Clock = std::chrono::steady_clock;

    // expirySeconds: peer không announce lại trong khoảng này sẽ bị coi là
    // đã rời mạng (mục 5.2: "thời gian hết hạn, vd 2 x interval").
    explicit PeerRegistry(int expirySeconds, std::size_t maxPeersPerTorrent);

    // now cho phép test giả lập thời gian trôi qua mà không cần sleep thật.
    void upsert(const std::string& infoHash, const PeerRegistration& peer,
                Clock::time_point now = Clock::now());
    void remove(const std::string& infoHash, const std::string& peerId);

    // Dọn mọi peer đã hết hạn trên toàn bộ registry.
    void purgeExpired(Clock::time_point now = Clock::now());

    [[nodiscard]] std::vector<TrackerPeer> getPeers(
        const std::string& infoHash,
        const std::string& excludePeerId = "",
        Clock::time_point now = Clock::now()) const;

    [[nodiscard]] bool hasPeer(const std::string& infoHash, const std::string& peerId) const;
    [[nodiscard]] std::size_t peerCount(const std::string& infoHash) const;

private:
    struct Entry {
        PeerRegistration peer;
        Clock::time_point lastSeen;
    };

    int expirySeconds_;
    std::size_t maxPeersPerTorrent_;
    std::map<std::string, std::vector<Entry>> swarms_;
};

}  // namespace p2p
