#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

// Node ID 160-bit theo BEP 5 (cùng kích thước info_hash: 20 byte / SHA-1).
class NodeId {
public:
    static constexpr std::size_t kByteLength = 20;
    static constexpr std::size_t kBitLength = kByteLength * 8;

    NodeId();  // Mặc định: toàn 0.
    explicit NodeId(std::array<std::uint8_t, kByteLength> bytes);

    // raw phải đúng 20 byte (không phải hex), giống info_hash/peer_id.
    static NodeId fromString(const std::string& raw);
    static NodeId fromHex(const std::string& hex);
    static NodeId random();

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toHex() const;

    [[nodiscard]] const std::array<std::uint8_t, kByteLength>& bytes() const { return bytes_; }

    bool operator==(const NodeId& other) const { return bytes_ == other.bytes_; }
    bool operator!=(const NodeId& other) const { return !(*this == other); }
    bool operator<(const NodeId& other) const { return bytes_ < other.bytes_; }

    // Khoảng cách Kademlia = XOR theo từng bit (BEP 5).
    [[nodiscard]] NodeId distanceTo(const NodeId& other) const;

    // Số bit tiền tố giống nhau giữa (*this) và other, dùng để chọn bucket
    // trong routing table (0 = khác nhau ngay bit đầu, kBitLength = trùng hệt).
    [[nodiscard]] std::size_t commonPrefixLength(const NodeId& other) const;

private:
    std::array<std::uint8_t, kByteLength> bytes_{};
};

}  // namespace p2p
