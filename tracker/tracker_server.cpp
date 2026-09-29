#include "tracker/tracker_server.h"

#include <algorithm>

namespace p2p {

TrackerServer::TrackerServer(int interval, std::size_t maxPeers)
    : interval_(interval), maxPeers_(maxPeers) {}

void TrackerServer::addPeer(const std::string& infoHash, const PeerRegistration& peer) {
    auto& list = peers_[infoHash];
    auto it = std::find_if(list.begin(), list.end(), [&](const PeerRegistration& candidate) {
        return candidate.peerId == peer.peerId;
    });

    if (it != list.end()) {
        *it = peer;
        return;
    }

    if (list.size() >= maxPeers_) {
        list.erase(list.begin());
    }
    list.push_back(peer);
}

void TrackerServer::removePeer(const std::string& infoHash, const std::string& peerId) {
    auto it = peers_.find(infoHash);
    if (it == peers_.end()) {
        return;
    }

    it->second.erase(
        std::remove_if(it->second.begin(), it->second.end(), [&](const PeerRegistration& peer) {
            return peer.peerId == peerId;
        }),
        it->second.end());

    if (it->second.empty()) {
        peers_.erase(it);
    }
}

std::vector<TrackerPeer> TrackerServer::getPeers(const std::string& infoHash) const {
    const auto it = peers_.find(infoHash);
    if (it == peers_.end()) {
        return {};
    }

    std::vector<TrackerPeer> peers;
    peers.reserve(it->second.size());
    for (const auto& peer : it->second) {
        peers.push_back({peer.ip, peer.port});
    }
    return peers;
}

bool TrackerServer::hasPeer(const std::string& infoHash, const std::string& peerId) const {
    const auto it = peers_.find(infoHash);
    if (it == peers_.end()) {
        return false;
    }

    return std::any_of(it->second.begin(), it->second.end(), [&](const PeerRegistration& peer) {
        return peer.peerId == peerId;
    });
}

std::string TrackerServer::announce(const TrackerRequest& request) {
    if (request.infoHash.empty()) {
        return TrackerProtocol::encodeFailureResponse("Missing info_hash");
    }

    if (request.event == "stopped") {
        removePeer(request.infoHash, request.peerId);
        return TrackerProtocol::encodeFailureResponse("Peer stopped");
    }

    const PeerRegistration peer{
        request.peerId,
        request.ip.empty() ? "127.0.0.1" : request.ip,
        request.port,
        request.uploaded,
        request.downloaded,
        request.left};

    addPeer(request.infoHash, peer);

    std::vector<TrackerPeer> peers = getPeers(request.infoHash);
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const TrackerPeer& peerInfo) {
        return peerInfo.ip == peer.ip && peerInfo.port == peer.port;
    }), peers.end());

    return TrackerProtocol::encodeSuccessResponse(interval_, peers);
}

}  // namespace p2p
