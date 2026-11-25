#include "network/dht/routing_table.h"

#include <algorithm>
#include <cstdio>

namespace p2p {

RoutingTable::RoutingTable(NodeId selfId, std::size_t bucketSize)
    : selfId_(selfId),
      bucketSize_(bucketSize),
      buckets_(NodeId::kBitLength + 1),
      replacements_(NodeId::kBitLength + 1) {}

namespace {

// "a.b.c" của "a.b.c.d"; rỗng nếu không phải IPv4 dạng chấm.
std::string subnet24(const std::string& ip) {
    const auto lastDot = ip.rfind('.');
    return lastDot == std::string::npos ? std::string() : ip.substr(0, lastDot);
}

// Chỉ IP công khai mới bị giới hạn theo /24 (LAN/loopback: nhiều máy cùng /24 là bình thường).
bool isPublicIpv4(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    if (a == 0 || a == 127 || a >= 224) return false;
    if (a == 10) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 169 && b == 254) return false;
    if (a == 100 && b >= 64 && b <= 127) return false;
    return true;
}

}  // namespace

void RoutingTable::insert(const DhtContact& contact) {
    if (contact.id == selfId_) {
        return;  // không tự thêm mình vào routing table
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t bucketIndex = selfId_.commonPrefixLength(contact.id);
    auto& bucket = buckets_[bucketIndex];

    const auto it = std::find_if(bucket.begin(), bucket.end(), [&](const DhtContact& existing) {
        return existing.id == contact.id;
    });

    if (it != bucket.end()) {
        // Đã biết node này: refresh thông tin + đẩy xuống cuối (coi là "mới nhất").
        bucket.erase(it);
        bucket.push_back(contact);
        return;
    }

    if (isPublicIpv4(contact.ip)) {
        const std::string subnet = subnet24(contact.ip);
        const auto sameSubnet = std::count_if(bucket.begin(), bucket.end(), [&](const DhtContact& existing) {
            return subnet24(existing.ip) == subnet;
        });
        if (static_cast<std::size_t>(sameSubnet) >= kMaxPerSubnet) {
            return;  // quá nhiều contact cùng /24 trong bucket này
        }
    }

    if (bucket.size() >= bucketSize_) {
        // Bucket đầy: giữ contact cũ (BEP 5), contact mới chỉ vào danh sách thay thế nhỏ.
        auto& spare = replacements_[bucketIndex];
        const auto dup = std::find_if(spare.begin(), spare.end(), [&](const DhtContact& existing) {
            return existing.id == contact.id;
        });
        if (dup != spare.end()) {
            spare.erase(dup);
        }
        spare.push_back(contact);
        if (spare.size() > kMaxReplacements) {
            spare.erase(spare.begin());
        }
        return;
    }
    bucket.push_back(contact);
}

void RoutingTable::remove(const NodeId& nodeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t bucketIndex = selfId_.commonPrefixLength(nodeId);
    auto& bucket = buckets_[bucketIndex];
    const auto before = bucket.size();
    bucket.erase(
        std::remove_if(bucket.begin(), bucket.end(), [&](const DhtContact& contact) {
            return contact.id == nodeId;
        }),
        bucket.end());
    auto& spare = replacements_[bucketIndex];
    spare.erase(std::remove_if(spare.begin(), spare.end(), [&](const DhtContact& contact) {
                    return contact.id == nodeId;
                }),
                spare.end());
    // Có chỗ trống: đôn contact thay thế mới nhất lên.
    if (bucket.size() < before && !spare.empty() && bucket.size() < bucketSize_) {
        bucket.push_back(spare.back());
        spare.pop_back();
    }
}

std::vector<DhtContact> RoutingTable::closestTo(const NodeId& target, std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DhtContact> all;
    for (const auto& bucket : buckets_) {
        all.insert(all.end(), bucket.begin(), bucket.end());
    }

    std::sort(all.begin(), all.end(), [&](const DhtContact& a, const DhtContact& b) {
        return a.id.distanceTo(target) < b.id.distanceTo(target);
    });

    if (all.size() > count) {
        all.resize(count);
    }
    return all;
}

std::size_t RoutingTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& bucket : buckets_) {
        total += bucket.size();
    }
    return total;
}

bool RoutingTable::contains(const NodeId& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t bucketIndex = selfId_.commonPrefixLength(nodeId);
    const auto& bucket = buckets_[bucketIndex];
    return std::any_of(bucket.begin(), bucket.end(), [&](const DhtContact& contact) {
        return contact.id == nodeId;
    });
}

}  // namespace p2p
