#pragma once
#include <vector>

namespace p2p {
namespace strategy {

class PiecePicker {
public:
    virtual ~PiecePicker() = default;
    
    // Nhận vào danh sách các mảnh mình đang thiếu, và bản đồ mảnh của các Peer
    // Trả về index của mảnh nên tải tiếp theo
    virtual int pick_next_piece(const std::vector<bool>& my_bitfield, 
                                const std::vector<std::vector<bool>>& peers_bitfields) = 0;
};

// Cài đặt chiến lược Rarest First
class RarestFirstPicker : public PiecePicker {
public:
    int pick_next_piece(const std::vector<bool>& my_bitfield, 
                        const std::vector<std::vector<bool>>& peers_bitfields) override;
};

}
}