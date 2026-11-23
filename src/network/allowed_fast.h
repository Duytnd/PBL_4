#pragma once

#include "core/hash_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbl4::network {

// BEP 6: tập "Allowed Fast" chuẩn. Peer được phép xin các piece này kể cả khi đang bị choke, nên tập phải tất định theo
// (IPv4 của peer, info-hash) để cả hai bên cùng tính ra và không ai chọn được tập có lợi cho mình.
//   x = SHA1( (ip & 0xFFFFFF00) || info_hash )
//   lặp: mỗi lần băm cho 5 chỉ số 4 byte (big-endian) % num_pieces, bỏ trùng; x = SHA1(x) cho tới khi đủ k.
// `ip_host_order` là IPv4 dạng số (a.b.c.d -> a<<24|b<<16|c<<8|d). `info_hash` đúng 20 byte thô.
inline std::vector<std::uint32_t> compute_allowed_fast_set(
    std::uint32_t ip_host_order, const std::string& info_hash, std::uint32_t num_pieces, std::size_t k) {
    std::vector<std::uint32_t> result;
    if (num_pieces == 0 || info_hash.size() != 20) {
        return result;
    }
    if (k > num_pieces) {
        k = num_pieces;
    }
    const std::uint32_t masked = ip_host_order & 0xFFFFFF00u;
    std::string x;
    x.push_back(static_cast<char>((masked >> 24) & 0xFF));
    x.push_back(static_cast<char>((masked >> 16) & 0xFF));
    x.push_back(static_cast<char>((masked >> 8) & 0xFF));
    x.push_back(static_cast<char>(masked & 0xFF));
    x += info_hash;
    while (result.size() < k) {
        x = p2p::sha1Digest(x);
        for (std::size_t i = 0; i < 5 && result.size() < k; ++i) {
            const auto byte = [&](std::size_t n) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(x[i * 4 + n]));
            };
            const std::uint32_t index = ((byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3)) % num_pieces;
            bool seen = false;
            for (std::uint32_t existing : result) {
                if (existing == index) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                result.push_back(index);
            }
        }
    }
    return result;
}

}  // namespace pbl4::network
