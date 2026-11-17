#pragma once

#include "strategy/peer_view.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p2p {
namespace strategy {

// Tham số cho PeerPolicy.
struct PeerPolicyConfig {
    // Số kết nối tối đa cho phép đồng thời với một torrent.
    std::size_t max_connections = 50;

    // Bao nhiêu lần gửi sai hash piece thì bị ban vĩnh viễn (trong phiên này).
    int bad_piece_threshold = 3;
};

// PeerPolicy — Kiểm soát kết nối và phòng thủ:
//  1. Giới hạn tổng số kết nối tối đa cho phép.
//  2. Ghi nhận và ban peer gửi piece sai SHA-1 nhiều lần.
//
// Thread-safe: mọi thành viên public đều có thể gọi từ nhiều luồng.
// Session giữ một instance duy nhất, gọi report_bad_piece() khi verify thất bại
// và is_banned() trước khi chấp nhận kết nối mới.
class PeerPolicy {
public:
    explicit PeerPolicy(PeerPolicyConfig config = PeerPolicyConfig());

    // ── Kiểm tra trước khi chấp nhận kết nối mới ──────────────────────────
    // Trả true nếu địa chỉ đã bị ban (gửi sai hash quá nhiều lần).
    bool is_banned(const std::string& address) const;

    // Trả true nếu vượt giới hạn kết nối tối đa.
    bool at_connection_limit(std::size_t current_connections) const;

    // ── Ghi nhận sự kiện từ TorrentSession ────────────────────────────────
    // Tăng bộ đếm bad-piece cho peer; nếu vượt threshold thì ban luôn.
    // Trả true nếu peer vừa bị ban trong lần gọi này.
    bool report_bad_piece(const std::string& address);

    // ── Thông tin trạng thái (debug / Web UI) ─────────────────────────────
    // Danh sách địa chỉ hiện đang bị ban.
    std::vector<std::string> banned_addresses() const;

    // Số lần gửi sai hash của một địa chỉ (0 nếu chưa từng).
    int bad_piece_count(const std::string& address) const;

    // Xoá toàn bộ danh sách ban (dùng cho test hoặc reset session).
    void clear_bans();

    const PeerPolicyConfig& config() const { return config_; }

private:
    PeerPolicyConfig config_;

    mutable std::mutex mutex_;
    std::unordered_set<std::string> banned_;       // địa chỉ bị ban vĩnh viễn
    std::unordered_map<std::string, int> strikes_;  // address → số lần sai hash
};

}  // namespace strategy
}  // namespace p2p

