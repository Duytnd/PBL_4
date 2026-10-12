#include "network/dht/node_id.h"

#include <random>
#include <stdexcept>

namespace p2p {

NodeId::NodeId() = default;

NodeId::NodeId(std::array<std::uint8_t, kByteLength> bytes) : bytes_(bytes) {}

NodeId NodeId::fromString(const std::string& raw) {
    if (raw.size() != kByteLength) {
        throw std::invalid_argument("NodeId: raw id must be exactly 20 bytes");
    }
    std::array<std::uint8_t, kByteLength> bytes{};
    for (std::size_t i = 0; i < kByteLength; ++i) {
        bytes[i] = static_cast<std::uint8_t>(raw[i]);
    }
    return NodeId(bytes);
}

NodeId NodeId::fromHex(const std::string& hex) {
    if (hex.size() != kByteLength * 2) {
        throw std::invalid_argument("NodeId: hex id must be exactly 40 hex characters");
    }

    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("NodeId: invalid hex character");
    };

    std::array<std::uint8_t, kByteLength> bytes{};
    for (std::size_t i = 0; i < kByteLength; ++i) {
        const int high = hexValue(hex[i * 2]);
        const int low = hexValue(hex[i * 2 + 1]);
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return NodeId(bytes);
}

NodeId NodeId::random() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    std::array<std::uint8_t, kByteLength> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(dist(rng));
    }
    return NodeId(bytes);
}

std::string NodeId::toString() const {
    return std::string(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
}

std::string NodeId::toHex() const {
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(kByteLength * 2);
    for (const std::uint8_t byte : bytes_) {
        hex.push_back(hexDigits[(byte >> 4) & 0x0F]);
        hex.push_back(hexDigits[byte & 0x0F]);
    }
    return hex;
}

NodeId NodeId::distanceTo(const NodeId& other) const {
    std::array<std::uint8_t, kByteLength> result{};
    for (std::size_t i = 0; i < kByteLength; ++i) {
        result[i] = static_cast<std::uint8_t>(bytes_[i] ^ other.bytes_[i]);
    }
    return NodeId(result);
}

std::size_t NodeId::commonPrefixLength(const NodeId& other) const {
    for (std::size_t byteIndex = 0; byteIndex < kByteLength; ++byteIndex) {
        const std::uint8_t xored = static_cast<std::uint8_t>(bytes_[byteIndex] ^ other.bytes_[byteIndex]);
        if (xored == 0) {
            continue;
        }
        // Byte đầu tiên khác nhau: đếm số bit 0 liên tiếp từ bit cao nhất của nó.
        std::size_t bitInByte = 0;
        for (int bit = 7; bit >= 0; --bit) {
            if ((xored >> bit) & 0x01) {
                break;
            }
            ++bitInByte;
        }
        return byteIndex * 8 + bitInByte;
    }
    return kBitLength;  // giống hệt nhau
}

}  // namespace p2p
