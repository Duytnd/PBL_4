#include "tracker/tracker_server.h"

namespace p2p {

TrackerServer::TrackerServer(int interval, std::size_t maxPeers)
    : interval_(interval), registry_(interval * 2, maxPeers) {}

void TrackerServer::addPeer(const std::string& infoHash, const PeerRegistration& peer) {
    registry_.upsert(infoHash, peer);
}

void TrackerServer::removePeer(const std::string& infoHash, const std::string& peerId) {
    registry_.remove(infoHash, peerId);
}

std::vector<TrackerPeer> TrackerServer::getPeers(const std::string& infoHash) const {
    return registry_.getPeers(infoHash);
}

bool TrackerServer::hasPeer(const std::string& infoHash, const std::string& peerId) const {
    return registry_.hasPeer(infoHash, peerId);
}

PeerRegistry& TrackerServer::registry() {
    return registry_;
}

std::string TrackerServer::announce(const TrackerRequest& request) {
    if (request.infoHash.size() != 20) {
        return TrackerProtocol::encodeFailureResponse("info_hash must be 20 bytes");
    }
    if (request.peerId.size() != 20) {
        return TrackerProtocol::encodeFailureResponse("peer_id must be 20 bytes");
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

    // Không trả lại chính peer đang hỏi (mục 5.2).
    const std::vector<TrackerPeer> peers = registry_.getPeers(request.infoHash, request.peerId);

    return TrackerProtocol::encodeSuccessResponse(interval_, peers);
}

}  // namespace p2p
