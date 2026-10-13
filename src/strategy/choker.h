#pragma once

#include <vector>
#include <string>

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

// Cài đặt thuật toán Tit-for-tat cổ điển
class TitForTatChoker : public Choker {
private:
    int num_regular_slots = 3;  // Số lượng peer ưu tiên theo tốc độ
    int num_optimistic_slots = 1; // Số lượng peer chọn ngẫu nhiên
    
    // Lưu trữ peer đang được ưu tiên ngẫu nhiên để không thay đổi nó liên tục mỗi 10 giây
    std::string current_optimistic_peer; 
    
public:
    std::vector<std::string> update_chokes(const std::vector<PeerStatus>& peers, bool is_seeding_only) override;
};

} // namespace strategy
} // namespace p2p