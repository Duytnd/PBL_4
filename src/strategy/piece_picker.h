#pragma once

#include <cstddef>
#include <random>
#include <vector>

namespace p2p {
namespace strategy {

// Ngữ cảnh tiến độ tải, do TorrentSession điền vào mỗi lần hỏi picker.
// Picker là logic thuần: không đọc trạng thái của session, mọi thứ cần biết nằm ở đây.
struct PickerContext {
    std::size_t total_pieces = 0;        // tổng số piece của torrent
    std::size_t completed_pieces = 0;    // piece đã đủ block và verify SHA-1 xong
    std::size_t in_progress_pieces = 0;  // piece đã bắt đầu xin nhưng chưa đủ block
};

class PiecePicker {
public:
    virtual ~PiecePicker() = default;

    // GIAO DIỆN CŨ (giữ nguyên để code hiện có không bị vỡ).
    // Nhận vào danh sách các mảnh mình đang thiếu, và bản đồ mảnh của các Peer.
    // Trả về index của mảnh nên tải tiếp theo, hoặc -1 nếu không có mảnh nào chọn được.
    virtual int pick_next_piece(const std::vector<bool>& my_bitfield,
                                const std::vector<std::vector<bool>>& peers_bitfields) = 0;

    // GIAO DIỆN MỚI: có thêm ngữ cảnh tiến độ (cho "piece đầu tiên ngẫu nhiên", endgame...).
    // Mặc định gọi lại hàm cũ, nên chiến lược nào chưa cần ngữ cảnh thì không phải viết thêm gì.
    virtual int pick_piece(const PickerContext& ctx,
                           const std::vector<bool>& my_bitfield,
                           const std::vector<std::vector<bool>>& peers_bitfields) {
        (void)ctx;
        return pick_next_piece(my_bitfield, peers_bitfields);
    }

    // Đang ở endgame chưa? (mọi piece còn thiếu đều đã có người xin.)
    // Khi true, session được phép xin trùng một block từ nhiều peer rồi cancel phần thừa.
    virtual bool in_endgame(const PickerContext& ctx) const {
        (void)ctx;
        return false;
    }

    // Số request chạy song song tối đa trên mỗi peer (pipeline).
    virtual std::size_t pipeline_depth() const { return 4; }

    // Một block được phép xin cùng lúc từ tối đa bao nhiêu peer khi đang ở endgame.
    // 1 = không xin trùng.
    virtual std::size_t endgame_max_copies() const { return 1; }
};

// Tham số của Rarest First, để thí nghiệm đổi chiến lược mà không sửa code (mục 5.5).
struct RarestFirstConfig {
    // Khi mình có ít hơn số piece này thì chọn NGẪU NHIÊN thay vì rarest-first,
    // để có dữ liệu chia sẻ cho peer khác sớm. 0 = tắt.
    std::size_t random_first_pieces = 4;
    // Số request song song mỗi peer.
    std::size_t pipeline_depth = 4;
    // Bật/tắt endgame.
    bool endgame_enabled = true;
    // Số peer tối đa được xin cùng một block khi ở endgame.
    std::size_t endgame_max_copies = 2;
};

// Cài đặt chiến lược Rarest First:
//  - Ưu tiên piece hiếm nhất trong số các peer đang kết nối (hòa thì chọn ngẫu nhiên).
//  - Những piece đầu tiên chọn ngẫu nhiên (xem random_first_pieces).
//  - Có endgame và pipeline cấu hình được.
// Lưu ý: không thread-safe; TorrentSession gọi picker khi đang giữ session_mutex_.
class RarestFirstPicker : public PiecePicker {
public:
    explicit RarestFirstPicker(RarestFirstConfig config = RarestFirstConfig(),
                               unsigned int seed = std::random_device{}());

    int pick_next_piece(const std::vector<bool>& my_bitfield,
                        const std::vector<std::vector<bool>>& peers_bitfields) override;

    int pick_piece(const PickerContext& ctx,
                   const std::vector<bool>& my_bitfield,
                   const std::vector<std::vector<bool>>& peers_bitfields) override;

    bool in_endgame(const PickerContext& ctx) const override;

    std::size_t pipeline_depth() const override;
    std::size_t endgame_max_copies() const override;

private:
    // random_mode = true: chọn ngẫu nhiên trong mọi piece có thể tải;
    // false: chọn trong nhóm piece hiếm nhất.
    int choose(const std::vector<bool>& my_bitfield,
               const std::vector<std::vector<bool>>& peers_bitfields,
               bool random_mode);

    RarestFirstConfig config_;
    std::mt19937 rng_;
};

}  // namespace strategy
}  // namespace p2p
