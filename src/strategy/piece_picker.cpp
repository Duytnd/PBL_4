#include "piece_picker.h"

#include <algorithm>
#include <limits>

namespace p2p {
namespace strategy {

RarestFirstPicker::RarestFirstPicker(RarestFirstConfig config, unsigned int seed)
    : config_(config), rng_(seed) {}

int RarestFirstPicker::pick_next_piece(const std::vector<bool>& my_bitfield,
                                       const std::vector<std::vector<bool>>& peers_bitfields) {
    // Không có ngữ cảnh: chạy rarest-first thuần như trước đây.
    return choose(my_bitfield, peers_bitfields, false);
}

int RarestFirstPicker::pick_piece(const PickerContext& ctx,
                                  const std::vector<bool>& my_bitfield,
                                  const std::vector<std::vector<bool>>& peers_bitfields) {
    const bool random_mode = ctx.completed_pieces < config_.random_first_pieces;
    return choose(my_bitfield, peers_bitfields, random_mode);
}

int RarestFirstPicker::choose(const std::vector<bool>& my_bitfield,
                              const std::vector<std::vector<bool>>& peers_bitfields,
                              bool random_mode) {
    if (my_bitfield.empty()) return -1;

    const std::size_t num_pieces = my_bitfield.size();
    std::vector<int> piece_counts(num_pieces, 0);

    // Bước 1: Khảo sát thị trường - đếm số peer đang giữ từng mảnh.
    for (const auto& peer_bf : peers_bitfields) {
        if (peer_bf.size() != num_pieces) continue;  // Bỏ qua dữ liệu lỗi
        for (std::size_t i = 0; i < num_pieces; ++i) {
            if (peer_bf[i]) {
                piece_counts[i]++;
            }
        }
    }

    // Bước 2: Lọc ứng viên. Chỉ quan tâm mảnh mình chưa có và có ít nhất 1 peer giữ.
    //  - Chế độ ngẫu nhiên: mọi mảnh như vậy đều là ứng viên.
    //  - Chế độ rarest-first: chỉ những mảnh có số người giữ nhỏ nhất.
    int min_count = std::numeric_limits<int>::max();
    std::vector<int> candidates;

    for (std::size_t i = 0; i < num_pieces; ++i) {
        if (my_bitfield[i] || piece_counts[i] <= 0) continue;

        if (random_mode) {
            candidates.push_back(static_cast<int>(i));
        } else if (piece_counts[i] < min_count) {
            min_count = piece_counts[i];
            candidates.clear();
            candidates.push_back(static_cast<int>(i));
        } else if (piece_counts[i] == min_count) {
            candidates.push_back(static_cast<int>(i));
        }
    }

    // Bước 3: Đã tải xong hết, hoặc không peer nào có mảnh mình cần.
    if (candidates.empty()) return -1;

    // Bước 4: Chọn ngẫu nhiên trong nhóm ứng viên để các peer không đổ xô vào cùng một mảnh.
    std::uniform_int_distribution<std::size_t> distrib(0, candidates.size() - 1);
    return candidates[distrib(rng_)];
}

bool RarestFirstPicker::in_endgame(const PickerContext& ctx) const {
    if (!config_.endgame_enabled) return false;
    if (ctx.total_pieces == 0) return false;
    if (ctx.completed_pieces >= ctx.total_pieces) return false;  // đã xong, không còn gì để xin

    // Endgame: không còn piece nào chưa được bắt đầu xin, chỉ còn các piece đang dở.
    return ctx.completed_pieces + ctx.in_progress_pieces >= ctx.total_pieces;
}

std::size_t RarestFirstPicker::pipeline_depth() const {
    return std::max<std::size_t>(1, config_.pipeline_depth);
}

std::size_t RarestFirstPicker::endgame_max_copies() const {
    if (!config_.endgame_enabled) return 1;
    return std::max<std::size_t>(1, config_.endgame_max_copies);
}

}  // namespace strategy
}  // namespace p2p
