#pragma once

#include "network/tracker_protocol.h"

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

class TrackerServer {
public:
    explicit TrackerServer(int interval = 1800, std::size_t maxPeers = 50);

    void addPeer(const std::string& infoHash, const PeerRegistration& peer);
    void removePeer(const std::string& infoHash, const std::string& peerId);
    std::string announce(const TrackerRequest& request);

    [[nodiscard]] std::vector<TrackerPeer> getPeers(const std::string& infoHash) const;
    [[nodiscard]] bool hasPeer(const std::string& infoHash, const std::string& peerId) const;

private:
    int interval_;
    std::size_t maxPeers_;
    std::map<std::string, std::vector<PeerRegistration>> peers_;
};

}  // namespace p2p
