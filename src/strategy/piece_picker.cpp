#include "piece_picker.h"
#include <algorithm>
#include <random>
#include <limits>

namespace p2p {
namespace strategy {

int RarestFirstPicker::pick_next_piece(const std::vector<bool>& my_bitfield, 
                                       const std::vector<std::vector<bool>>& peers_bitfields) {
    if (my_bitfield.empty()) return -1;
    
    int num_pieces = my_bitfield.size();
    std::vector<int> piece_counts(num_pieces, 0);

    // Bước 1: Khảo sát thị trường - Đếm số lượng của từng mảnh đang có mặt trong mạng
    for (const auto& peer_bf : peers_bitfields) {
        if (peer_bf.size() != num_pieces) continue; // Bỏ qua nếu dữ liệu lỗi
        for (int i = 0; i < num_pieces; ++i) {
            if (peer_bf[i]) {
                piece_counts[i]++;
            }
        }
    }

    // Bước 2: Lọc ra các mảnh mình CƯA CÓ, và tìm tần suất xuất hiện NHỎ NHẤT (>0)
    int min_count = std::numeric_limits<int>::max();
    std::vector<int> rarest_pieces;

    for (int i = 0; i < num_pieces; ++i) {
        // Chỉ quan tâm mảnh mình chưa có và có ít nhất 1 peer đang giữ
        if (!my_bitfield[i] && piece_counts[i] > 0) {
            if (piece_counts[i] < min_count) {
                // Tìm thấy độ hiếm mới, xóa danh sách cũ đi
                min_count = piece_counts[i];
                rarest_pieces.clear();
                rarest_pieces.push_back(i);
            } else if (piece_counts[i] == min_count) {
                // Mảnh này cũng hiếm ngang ngửa, đưa vào danh sách ứng viên
                rarest_pieces.push_back(i);
            }
        }
    }

    // Bước 3: Chốt hạ
    if (rarest_pieces.empty()) {
        return -1; // Đã tải xong hết, hoặc mạng không ai có mảnh mình cần
    }

    // Bước 4: Nếu có nhiều mảnh cùng độ hiếm, chọn ngẫu nhiên 1 mảnh để tránh các Peer cùng đổ xô tải 1 mảnh
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, rarest_pieces.size() - 1);

    return rarest_pieces[distrib(gen)];
}

} // namespace strategy
} // namespace p2p