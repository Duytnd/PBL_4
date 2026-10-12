#include "tracker/peer_registry.h"

#include <algorithm>

namespace p2p {

PeerRegistry::PeerRegistry(int expirySeconds, std::size_t maxPeersPerTorrent)
    : expirySeconds_(expirySeconds), maxPeersPerTorrent_(maxPeersPerTorrent) {}

void PeerRegistry::upsert(const std::string& infoHash, const PeerRegistration& peer, Clock::time_point now) {
    auto& entries = swarms_[infoHash];
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
        return entry.peer.peerId == peer.peerId;
    });

    if (it != entries.end()) {
        it->peer = peer;
        it->lastSeen = now;
        return;
    }

    if (entries.size() >= maxPeersPerTorrent_) {
        // Hết chỗ: nhường cho peer mới bằng cách bỏ peer cũ nhất (đã announce lâu nhất rồi).
        const auto oldest = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.lastSeen < b.lastSeen;
        });
        entries.erase(oldest);
    }

    entries.push_back(Entry{peer, now});
}

void PeerRegistry::remove(const std::string& infoHash, const std::string& peerId) {
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
    const auto it = swarms_.find(infoHash);
    if (it == swarms_.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [&](const Entry& entry) {
        return entry.peer.peerId == peerId;
    });
}

std::size_t PeerRegistry::peerCount(const std::string& infoHash) const {
    const auto it = swarms_.find(infoHash);
    return it == swarms_.end() ? 0 : it->second.size();
}

}  // namespace p2p
