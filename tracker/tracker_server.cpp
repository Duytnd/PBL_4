#include "tracker/tracker_server.h"

namespace p2p {

TrackerServer::TrackerServer(int interval, std::size_t maxPeers)
    : interval_(interval), registry_(interval * 2, maxPeers) {}

bool TrackerServer::addPeer(const std::string& infoHash, const PeerRegistration& peer) {
    return registry_.upsert(infoHash, peer);
}

bool TrackerServer::registerUdpEndpoint(
    const std::string& infoHash,
    const std::string& peerId,
    const std::string& sourceIp,
    std::uint16_t sourcePort,
    std::uint16_t announcedTcpPort,
    const std::string& key) {
    return registry_.updateUdpEndpoint(
        infoHash, peerId, sourceIp, sourcePort, announcedTcpPort, PeerRegistry::Clock::now(), key);
}

std::optional<TrackerPeer> TrackerServer::rendezvousEndpoint(
    const std::string& infoHash,
    const std::string& requesterPeerId,
    const std::string& requesterIp,
    const std::string& targetIp,
    std::uint16_t targetTcpPort,
    const std::string& requesterKey) const {
    return registry_.getUdpEndpointByTcpEndpoint(
        infoHash, requesterPeerId, requesterIp, targetIp, targetTcpPort, PeerRegistry::Clock::now(), requesterKey);
}

void TrackerServer::removePeer(const std::string& infoHash, const std::string& peerId, const std::string& key) {
    registry_.remove(infoHash, peerId, key);
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

    // Khoá phiên: hex đúng 16 ký tự -> 8 byte thô. Vắng mặt = client cũ. Có mà sai định dạng = từ chối.
    std::string rawKey;
    if (!request.key.empty()) {
        if (request.key.size() != 16 || request.key.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            return TrackerProtocol::encodeFailureResponse("key must be 16 hex characters");
        }
        for (std::size_t i = 0; i < 16; i += 2) {
            rawKey.push_back(static_cast<char>(std::stoi(request.key.substr(i, 2), nullptr, 16)));
        }
    }

    if (request.event == "stopped") {
        removePeer(request.infoHash, request.peerId, rawKey);
        return TrackerProtocol::encodeFailureResponse("Peer stopped");
    }

    const PeerRegistration peer{
        request.peerId,
        request.ip.empty() ? "127.0.0.1" : request.ip,
        request.port,
        request.uploaded,
        request.downloaded,
        request.left,
        rawKey};

    // Compact peer list (BEP 23) chỉ mã hoá được IPv4; một peer IPv6 sẽ làm hỏng cả swarm.
    if (peer.ip.find(':') != std::string::npos) {
        return TrackerProtocol::encodeFailureResponse("IPv6 peers are not supported");
    }
    switch (registry_.upsertChecked(request.infoHash, peer)) {
        case PeerRegistry::UpsertStatus::Ok:
            break;
        case PeerRegistry::UpsertStatus::KeyMismatch:
            return TrackerProtocol::encodeFailureResponse("peer_id is already in use by another client");
        case PeerRegistry::UpsertStatus::SwarmFull:
            return TrackerProtocol::encodeFailureResponse("Tracker is full");
    }

    // Không trả lại chính peer đang hỏi (mục 5.2).
    const std::vector<TrackerPeer> peers = registry_.getPeers(request.infoHash, request.peerId);

    return TrackerProtocol::encodeSuccessResponse(interval_, peers);
}

}  // namespace p2p
