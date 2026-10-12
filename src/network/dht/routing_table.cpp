#include "network/dht/routing_table.h"

#include <algorithm>

namespace p2p {

RoutingTable::RoutingTable(NodeId selfId, std::size_t bucketSize)
    : selfId_(selfId), bucketSize_(bucketSize), buckets_(NodeId::kBitLength + 1) {}

void RoutingTable::insert(const DhtContact& contact) {
    if (contact.id == selfId_) {
        return;  // không tự thêm mình vào routing table
    }

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

    if (bucket.size() >= bucketSize_) {
        // Bucket đầy: loại contact cũ nhất (đầu danh sách) để nhường chỗ.
        bucket.erase(bucket.begin());
    }
    bucket.push_back(contact);
}

void RoutingTable::remove(const NodeId& nodeId) {
    const std::size_t bucketIndex = selfId_.commonPrefixLength(nodeId);
    auto& bucket = buckets_[bucketIndex];
    bucket.erase(
        std::remove_if(bucket.begin(), bucket.end(), [&](const DhtContact& contact) {
            return contact.id == nodeId;
        }),
        bucket.end());
}

std::vector<DhtContact> RoutingTable::closestTo(const NodeId& target, std::size_t count) const {
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
    std::size_t total = 0;
    for (const auto& bucket : buckets_) {
        total += bucket.size();
    }
    return total;
}

bool RoutingTable::contains(const NodeId& nodeId) const {
    const std::size_t bucketIndex = selfId_.commonPrefixLength(nodeId);
    const auto& bucket = buckets_[bucketIndex];
    return std::any_of(bucket.begin(), bucket.end(), [&](const DhtContact& contact) {
        return contact.id == nodeId;
    });
}

}  // namespace p2p
