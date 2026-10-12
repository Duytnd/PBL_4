#pragma once

#include "network/dht/node_id.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct DhtContact {
    NodeId id;
    std::string ip;
    std::uint16_t port = 0;

    bool operator==(const DhtContact& other) const {
        return id == other.id && ip == other.ip && port == other.port;
    }
};

// Routing table Kademlia: k = 8 contact mỗi bucket (BEP 5, mục 5.6).
//
// Cách cài: mỗi bucket ứng với 1 giá trị commonPrefixLength(self, contact)
// (0..160). Đây tương đương với 1 cây k-bucket đã tách hết tới từng bit,
// chỉ khác là không tách dần (lazy splitting) để tối ưu bộ nhớ khi routing
// table còn thưa - với quy mô vài chục node ảo (mục 5.6: 15-30 peer ảo) thì
// 161 bucket cố định (160 bit + 1) không tốn kém đáng kể và cho đúng kết quả.
class RoutingTable {
public:
    static constexpr std::size_t kBucketSize = 8;

    explicit RoutingTable(NodeId selfId, std::size_t bucketSize = kBucketSize);

    // Thêm/refresh 1 contact. Không thêm chính mình. Khi bucket đầy, loại bỏ
    // contact cũ nhất (ít hoạt động nhất) để nhường chỗ (đơn giản hoá: không
    // ping lại contact cũ trước khi loại, khác hành vi Kademlia đầy đủ).
    void insert(const DhtContact& contact);
    void remove(const NodeId& nodeId);

    // Trả về tối đa "count" contact gần "target" nhất theo khoảng cách XOR.
    [[nodiscard]] std::vector<DhtContact> closestTo(const NodeId& target, std::size_t count) const;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool contains(const NodeId& nodeId) const;
    [[nodiscard]] const NodeId& selfId() const { return selfId_; }

private:
    NodeId selfId_;
    std::size_t bucketSize_;
    // index = commonPrefixLength(selfId_, contact.id), 0..NodeId::kBitLength
    std::vector<std::vector<DhtContact>> buckets_;
};

}  // namespace p2p
