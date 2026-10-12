#pragma once

#include "network/tracker_protocol.h"
#include "tracker/peer_registry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace p2p {

class TrackerServer {
public:
    // interval: giây giữa 2 lần announce định kỳ (trả cho client trong response).
    // Peer hết hạn sau 2 x interval nếu không announce lại (mục 5.2 kế hoạch).
    explicit TrackerServer(int interval = 1800, std::size_t maxPeers = 50);

    void addPeer(const std::string& infoHash, const PeerRegistration& peer);
    void removePeer(const std::string& infoHash, const std::string& peerId);
    std::string announce(const TrackerRequest& request);

    [[nodiscard]] std::vector<TrackerPeer> getPeers(const std::string& infoHash) const;
    [[nodiscard]] bool hasPeer(const std::string& infoHash, const std::string& peerId) const;

    // Cho test: xoá peer hết hạn ngay, không cần chờ thời gian thật trôi qua.
    [[nodiscard]] PeerRegistry& registry();

private:
    int interval_;
    PeerRegistry registry_;
};

}  // namespace p2p
