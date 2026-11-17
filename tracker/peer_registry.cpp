#include "tracker/peer_registry.h"

#include <algorithm>

namespace p2p {

PeerRegistry::PeerRegistry(int expirySeconds, std::size_t maxPeersPerTorrent, std::size_t maxSwarms)
    : expirySeconds_(expirySeconds), maxPeersPerTorrent_(maxPeersPerTorrent), maxSwarms_(maxSwarms) {}

bool PeerRegistry::upsert(const std::string& infoHash, const PeerRegistration& peer, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (swarms_.find(infoHash) == swarms_.end() && swarms_.size() >= maxSwarms_) {
        return false;
    }
    auto& entries = swarms_[infoHash];
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
        return entry.peer.peerId == peer.peerId;
    });

    if (it != entries.end()) {
        it->peer = peer;
        it->lastSeen = now;
        return true;
    }

    if (entries.size() >= maxPeersPerTorrent_) {
        // Hết chỗ: nhường cho peer mới bằng cách bỏ peer cũ nhất (đã announce lâu nhất rồi).
        const auto oldest = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.lastSeen < b.lastSeen;
        });
        entries.erase(oldest);
    }

    entries.push_back(Entry{peer, now, {}, 0, {}});
    return true;
}

void PeerRegistry::remove(const std::string& infoHash, const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = swarms_.find(infoHash);
    if (it == swarms_.end()) {
        return;
    }

    auto& entries = it->second;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.peer.peerId == peerId;
        }),
        entries.end());

    if (entries.empty()) {
        swarms_.erase(it);
    }
}

void PeerRegistry::purgeExpired(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto expiry = std::chrono::seconds(expirySeconds_);

    for (auto swarmIt = swarms_.begin(); swarmIt != swarms_.end();) {
        auto& entries = swarmIt->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) {
                return now - entry.lastSeen > expiry;
            }),
            entries.end());

        if (entries.empty()) {
            swarmIt = swarms_.erase(swarmIt);
        } else {
            ++swarmIt;
        }
    }
}

std::vector<TrackerPeer> PeerRegistry::getPeers(
    const std::string& infoHash, const std::string& excludePeerId, Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = swarms_.find(infoHash);
    if (it == swarms_.end()) {
        return {};
    }

    const auto expiry = std::chrono::seconds(expirySeconds_);
    std::vector<TrackerPeer> peers;
    peers.reserve(it->second.size());

    for (const auto& entry : it->second) {
        if (now - entry.lastSeen > expiry) {
            continue;  // đã hết hạn, coi như không còn trong mạng
        }
        if (entry.peer.peerId == excludePeerId) {
            continue;  // không trả lại chính peer đang hỏi
        }
        peers.push_back({entry.peer.ip, entry.peer.port});
    }

    return peers;
}

bool PeerRegistry::hasPeer(const std::string& infoHash, const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = swarms_.find(infoHash);
    if (it == swarms_.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [&](const Entry& entry) {
        return entry.peer.peerId == peerId;
    });
}

std::size_t PeerRegistry::peerCount(const std::string& infoHash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = swarms_.find(infoHash);
    return it == swarms_.end() ? 0 : it->second.size();
}

bool PeerRegistry::updateUdpEndpoint(
    const std::string& infoHash,
    const std::string& peerId,
    const std::string& sourceIp,
    std::uint16_t sourcePort,
    std::uint16_t announcedTcpPort,
    Clock::time_point now) {
    if (sourceIp.empty() || sourcePort == 0 || announcedTcpPort == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto swarm = swarms_.find(infoHash);
    if (swarm == swarms_.end()) {
        return false;
    }

    const auto entry = std::find_if(swarm->second.begin(), swarm->second.end(), [&](const Entry& candidate) {
        return candidate.peer.peerId == peerId;
    });
    if (entry == swarm->second.end() || entry->peer.ip != sourceIp ||
        entry->peer.port != announcedTcpPort ||
        now - entry->lastSeen > std::chrono::seconds(expirySeconds_)) {
        return false;
    }

    entry->udpIp = sourceIp;
    entry->udpPort = sourcePort;
    entry->udpLastSeen = now;
    return true;
}

std::optional<TrackerPeer> PeerRegistry::getUdpEndpointByTcpEndpoint(
    const std::string& infoHash,
    const std::string& requesterPeerId,
    const std::string& requesterIp,
    const std::string& targetIp,
    std::uint16_t targetTcpPort,
    Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto swarm = swarms_.find(infoHash);
    if (swarm == swarms_.end()) {
        return std::nullopt;
    }

    const auto& entries = swarm->second;
    const auto requester = std::find_if(entries.begin(), entries.end(), [&](const Entry& candidate) {
        return candidate.peer.peerId == requesterPeerId && candidate.peer.ip == requesterIp &&
               now - candidate.lastSeen <= std::chrono::seconds(expirySeconds_);
    });
    if (requester == entries.end()) {
        return std::nullopt;
    }

    const auto target = std::find_if(entries.begin(), entries.end(), [&](const Entry& candidate) {
        return candidate.peer.peerId != requesterPeerId && candidate.peer.ip == targetIp &&
               candidate.peer.port == targetTcpPort && candidate.udpPort != 0 &&
               now - candidate.udpLastSeen <= std::chrono::seconds(120) &&
               now - candidate.lastSeen <= std::chrono::seconds(expirySeconds_);
    });
    if (target == entries.end()) {
        return std::nullopt;
    }

    return TrackerPeer{target->udpIp, target->udpPort};
}

}  // namespace p2p
