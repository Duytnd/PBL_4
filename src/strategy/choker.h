#pragma once

#include <random>
#include <string>
#include <vector>

namespace p2p {
namespace strategy {

// CẢNH BÁO CHO NGƯỜI 2: Dữ liệu này do luồng mạng (Network thread) của Người 2
// liên tục cập nhật và truyền vào cho Người 3 tính toán.
struct PeerStatus {
    std::string peer_id;      // ID định danh của máy trạm
    bool is_interested;       // Máy kia có đang muốn tải dữ liệu từ mình không?
    bool am_choking_them;     // Mình có đang chặn máy kia tải không?
    double download_rate;     // Tốc độ mình đang tải TỪ máy kia (bytes/s) - Quan trọng nhất!
    double upload_rate;       // Tốc độ mình đang tải LÊN cho máy kia (bytes/s)
};

// Giao diện chung cho thuật toán chặn/mở kết nối
class Choker {
public:
    virtual ~Choker() = default;

    // Nhận vào danh sách trạng thái của tất cả Peer hiện tại.
    // Trả về danh sách các peer_id được phép tải (unchoke). Những ai không có tên sẽ bị chặn (choke).
    virtual std::vector<std::string> update_chokes(const std::vector<PeerStatus>& peers, bool is_seeding_only) = 0;
};

// Tham số của tit-for-tat, để thí nghiệm đổi chiến lược mà không sửa code (mục 5.5).
struct TitForTatConfig {
    // Số slot chọn theo tốc độ (mặc định 3).
    int regular_slots = 3;
    // Slot optimistic được giữ nguyên bao nhiêu vòng gọi update_chokes() rồi mới xoay.
    // update_chokes() được gọi mỗi 10 giây, nên 3 vòng = 30 giây (đúng mục 5.5).
    int optimistic_rotation_rounds = 3;
};

// Cài đặt thuật toán Tit-for-tat cổ điển:
//  - Mỗi vòng (10s): chọn `regular_slots` peer interested có tốc độ cao nhất.
//  - 1 slot optimistic unchoke: chọn ngẫu nhiên trong số peer interested còn lại,
//    giữ nguyên `optimistic_rotation_rounds` vòng rồi mới đổi người khác.
//  - Đang tải: xếp theo download_rate. Đang seed: xếp theo upload_rate.
class TitForTatChoker : public Choker {
public:
    explicit TitForTatChoker(TitForTatConfig config = TitForTatConfig(),
                             unsigned int seed = std::random_device{}());

    std::vector<std::string> update_chokes(const std::vector<PeerStatus>& peers, bool is_seeding_only) override;

    // Peer đang giữ slot optimistic (rỗng nếu chưa có). Dùng cho test và hiển thị.
    const std::string& current_optimistic_peer() const { return current_optimistic_peer_; }

private:
    TitForTatConfig config_;
    std::mt19937 rng_;

    // Peer đang được ưu tiên ngẫu nhiên, giữ ổn định giữa các vòng.
    std::string current_optimistic_peer_;
    // Số vòng liên tiếp peer optimistic hiện tại đã giữ slot.
    int rounds_held_ = 0;
};

} // namespace strategy
} // namespace p2p
