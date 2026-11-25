#pragma once

#include "network/dht/node_id.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
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
    static constexpr std::size_t kMaxPerSubnet = 2;      // contact công khai cùng /24 trong một bucket
    static constexpr std::size_t kMaxReplacements = 3;   // danh sách thay thế mỗi bucket

    explicit RoutingTable(NodeId selfId, std::size_t bucketSize = kBucketSize);

    // Thêm/refresh 1 contact. Không thêm chính mình. Theo BEP 5, bucket đầy thì GIỮ contact cũ (node sống lâu
    // đáng tin hơn node mới) và bỏ contact mới vào danh sách thay thế nhỏ; contact thay thế được đôn lên khi
    // một contact trong bucket bị remove(). Với IP công khai, mỗi bucket nhận tối đa kMaxPerSubnet contact cùng /24
    // (chặn một máy/mạng nhồi đầy bucket); IP private/loopback (LAN, test) không bị giới hạn này.
    void insert(const DhtContact& contact);
    void remove(const NodeId& nodeId);

    // Trả về tối đa "count" contact gần "target" nhất theo khoảng cách XOR.
    [[nodiscard]] std::vector<DhtContact> closestTo(const NodeId& target, std::size_t count) const;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool contains(const NodeId& nodeId) const;
    [[nodiscard]] const NodeId& selfId() const { return selfId_; }

private:
    mutable std::mutex mutex_;
    NodeId selfId_;
    std::size_t bucketSize_;
    // index = commonPrefixLength(selfId_, contact.id), 0..NodeId::kBitLength
    std::vector<std::vector<DhtContact>> buckets_;
    std::vector<std::vector<DhtContact>> replacements_;  // contact mới gặp khi bucket đầy (mới nhất ở cuối)
};

}  // namespace p2p
