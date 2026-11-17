#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {
namespace strategy {

// Snapshot trạng thái của một peer tại thời điểm session snapshot.
// Dùng để peer_policy ra quyết định mà không cần giữ lock vào session.
struct PeerView {
    std::string peer_id;         // ID định danh (20-byte hex hoặc dạng "-XX1234-...")
    std::string address;         // "ip:port" dạng chuỗi, dùng để log và ban
    double download_rate = 0.0;  // bytes/s session đang tải TỪ peer này
    double upload_rate   = 0.0;  // bytes/s session đang tải LÊN peer này
    int    bad_piece_count = 0;  // số lần peer gửi piece sai SHA-1
    bool   is_connected  = true; // còn kết nối không
};

}  // namespace strategy
}  // namespace p2p

