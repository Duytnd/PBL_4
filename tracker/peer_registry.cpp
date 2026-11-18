#include "tracker/peer_registry.h"

#include <algorithm>

namespace p2p {

PeerRegistry::PeerRegistry(int expirySeconds, std::size_t maxPeersPerTorrent, std::size_t maxSwarms,
                           std::size_t maxPeersPerIpPerSwarm, std::size_t maxSwarmsPerIp)
    : expirySeconds_(expirySeconds),
      maxPeersPerTorrent_(maxPeersPerTorrent),
      maxSwarms_(maxSwarms),
      maxPeersPerIpPerSwarm_(maxPeersPerIpPerSwarm),
      maxSwarmsPerIp_(maxSwarmsPerIp) {}

bool PeerRegistry::upsert(const std::string& infoHash, const PeerRegistration& peer, Clock::time_point now) {
    return upsertChecked(infoHash, peer, now) == UpsertStatus::Ok;
}

PeerRegistry::UpsertStatus PeerRegistry::upsertChecked(
    const std::string& infoHash, const PeerRegistration& peer, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (swarms_.find(infoHash) == swarms_.end()) {
        if (swarms_.size() >= maxSwarms_) {
            return UpsertStatus::SwarmFull;
        }
        // Bảng swarm đã đầy nửa: một IP không được chiếm quá maxSwarmsPerIp swarm (quét chỉ khi cần, không tốn lúc bình thường).
        if (swarms_.size() >= maxSwarms_ / 2 && maxSwarmsPerIp_ > 0) {
            std::size_t joined = 0;
            for (const auto& swarm : swarms_) {
                if (std::any_of(swarm.second.begin(), swarm.second.end(),
                                [&](const Entry& e) { return e.peer.ip == peer.ip; })) {
                    ++joined;
                }
            }
            if (joined >= maxSwarmsPerIp_) {
                return UpsertStatus::SwarmFull;
            }
        }
    }
    auto& entries = swarms_[infoHash];
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
        return entry.peer.peerId == peer.peerId;
    });

    if (it != entries.end()) {
        // peer_id này đã được giữ bằng 1 khoá: chỉ chủ khoá được cập nhật (kể cả khi đổi IP).
        if (!keyAuthorizes(it->peer.key, peer.key)) {
            return UpsertStatus::KeyMismatch;
        }
        it->peer = peer;
        it->lastSeen = now;
        return UpsertStatus::Ok;
    }

    // Giới hạn theo IP: IP này đã đủ chỗ thì thay peer cũ nhất của CHÍNH IP này, không động tới IP khác.
    if (maxPeersPerIpPerSwarm_ > 0) {
        std::size_t sameIp = 0;
        for (const auto& entry : entries) {
            sameIp += entry.peer.ip == peer.ip ? 1 : 0;
        }
        if (sameIp >= maxPeersPerIpPerSwarm_) {
            const auto oldestSameIp = std::min_element(entries.begin(), entries.end(),
                [&](const Entry& a, const Entry& b) {
                    const bool aSame = a.peer.ip == peer.ip;
                    const bool bSame = b.peer.ip == peer.ip;
                    if (aSame != bSame) return aSame;  // mục cùng IP xếp trước
                    return a.lastSeen < b.lastSeen;
                });
            entries.erase(oldestSameIp);
        }
    }

    if (entries.size() >= maxPeersPerTorrent_) {
        // Hết chỗ: nhường cho peer mới bằng cách bỏ peer cũ nhất (đã announce lâu nhất rồi).
        const auto oldest = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.lastSeen < b.lastSeen;
        });
        entries.erase(oldest);
    }

    entries.push_back(Entry{peer, now, {}, 0, {}});
    return UpsertStatus::Ok;
}

void PeerRegistry::remove(const std::string& infoHash, const std::string& peerId, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = swarms_.find(infoHash);
    if (it == swarms_.end()) {
        return;
    }

    auto& entries = it->second;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.peer.peerId == peerId && keyAuthorizes(entry.peer.key, key);
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
    Clock::time_point now,
    const std::string& key) {
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
    // Đúng IP và cổng TCP chưa đủ: người sau cùng NAT (cùng IP) biết peer_id của nạn nhân vẫn khớp hai điều đó.
    // Phải có thêm đúng khoá phiên thì mới đổi được endpoint UDP của peer.
    if (entry == swarm->second.end() || entry->peer.ip != sourceIp ||
        entry->peer.port != announcedTcpPort || !keyAuthorizes(entry->peer.key, key) ||
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
    Clock::time_point now,
    const std::string& requesterKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto swarm = swarms_.find(infoHash);
    if (swarm == swarms_.end()) {
        return std::nullopt;
    }

    const auto& entries = swarm->second;
    const auto requester = std::find_if(entries.begin(), entries.end(), [&](const Entry& candidate) {
        return candidate.peer.peerId == requesterPeerId && candidate.peer.ip == requesterIp &&
               keyAuthorizes(candidate.peer.key, requesterKey) &&
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
