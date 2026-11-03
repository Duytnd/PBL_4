#include "test_framework.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "strategy/piece_picker.h"

using namespace p2p::strategy;

namespace {

using Bits = std::vector<bool>;
using Swarm = std::vector<Bits>;

// Chiến lược tối giản chỉ cài hàm cũ: dùng để kiểm tra các hàm mặc định của lớp cơ sở.
class OldStylePicker : public PiecePicker {
public:
    int pick_next_piece(const Bits& mine, const Swarm&) override {
        for (std::size_t i = 0; i < mine.size(); ++i) {
            if (!mine[i]) return static_cast<int>(i);
        }
        return -1;
    }
};

PickerContext ctxOf(std::size_t total, std::size_t completed, std::size_t inProgress) {
    PickerContext c;
    c.total_pieces = total;
    c.completed_pieces = completed;
    c.in_progress_pieces = inProgress;
    return c;
}

// Tắt "piece đầu ngẫu nhiên" để kiểm tra rarest-first thuần.
RarestFirstConfig noRandomFirst() {
    RarestFirstConfig cfg;
    cfg.random_first_pieces = 0;
    return cfg;
}

}  // namespace

// ---------- Rarest-first ----------

void testRarestFirstPicksRarestPiece() {
    // Mình thiếu mảnh 1 và 3. Mảnh 1 có 2 peer giữ, mảnh 3 chỉ có 1 peer giữ.
    Bits mine = {true, false, true, false};
    Swarm peers = {
        {true, true, true, false},
        {false, false, false, false},
        {true, true, true, true},
    };
    RarestFirstPicker picker(noRandomFirst(), 1);
    CHECK_EQ(picker.pick_next_piece(mine, peers), 3);
}

void testPickNeverReturnsOwnedOrUnavailablePiece() {
    // Mảnh 0 mình đã có, mảnh 2 không ai giữ: chỉ còn 1 hoặc 3.
    Bits mine = {true, false, false, false};
    Swarm peers = {
        {true, true, false, true},
        {true, true, false, false},
    };
    for (unsigned seed = 0; seed < 100; ++seed) {
        RarestFirstPicker picker(noRandomFirst(), seed);
        const int p = picker.pick_next_piece(mine, peers);
        CHECK(p == 1 || p == 3);
    }
}

void testReturnsMinusOneWhenNothingToPick() {
    RarestFirstPicker picker(noRandomFirst(), 1);

    // Đã có hết.
    CHECK_EQ(picker.pick_next_piece(Bits{true, true}, Swarm{{true, true}}), -1);
    // Không còn peer nào.
    CHECK_EQ(picker.pick_next_piece(Bits{false, false}, Swarm{}), -1);
    // Peer không giữ mảnh nào mình thiếu.
    CHECK_EQ(picker.pick_next_piece(Bits{false, true}, Swarm{{false, true}}), -1);
    // Danh sách rỗng.
    CHECK_EQ(picker.pick_next_piece(Bits{}, Swarm{}), -1);
}

void testIgnoresPeerWithWrongSizedBitfield() {
    Bits mine = {false, false, false};
    Swarm peers = {
        {true, true},                // sai độ dài: phải bị bỏ qua
        {false, true, true},         // mảnh 1 và 2
        {false, false, true},        // mảnh 2
    };
    RarestFirstPicker picker(noRandomFirst(), 1);
    CHECK_EQ(picker.pick_next_piece(mine, peers), 1);  // mảnh 1 hiếm hơn mảnh 2
}

void testTiesAreBrokenRandomlyAndOnlyAmongRarest() {
    // Mảnh 0 và 1 cùng hiếm (1 người), mảnh 2 phổ biến (3 người).
    Bits mine = {false, false, false};
    Swarm peers = {
        {true, false, true},
        {false, true, true},
        {false, false, true},
    };
    std::set<int> seen;
    for (unsigned seed = 0; seed < 200; ++seed) {
        RarestFirstPicker picker(noRandomFirst(), seed);
        seen.insert(picker.pick_next_piece(mine, peers));
    }
    CHECK(seen.count(0) == 1);
    CHECK(seen.count(1) == 1);
    CHECK(seen.count(2) == 0);  // mảnh phổ biến không bao giờ được chọn
}

void testSameSeedGivesSameSequence() {
    Bits mine(16, false);
    Swarm peers = {Bits(16, true)};
    RarestFirstPicker a(RarestFirstConfig(), 123), b(RarestFirstConfig(), 123);
    for (int i = 0; i < 20; ++i) {
        CHECK_EQ(a.pick_piece(ctxOf(16, 0, 0), mine, peers),
                 b.pick_piece(ctxOf(16, 0, 0), mine, peers));
    }
}

void testDownloadsEveryPieceExactlyOnce() {
    // Mô phỏng một lượt tải: chọn rồi đánh dấu đã có, không được lặp hay bỏ sót piece nào.
    const std::size_t n = 40;
    Bits mine(n, false);
    Swarm peers = {Bits(n, true), Bits(n, true)};
    // Vài mảnh chỉ một peer giữ để mạng không đồng đều.
    for (std::size_t i = 0; i < n; i += 3) peers[1][i] = false;

    RarestFirstPicker picker(RarestFirstConfig(), 7);
    std::set<int> picked;
    for (std::size_t step = 0; step < n; ++step) {
        const int p = picker.pick_piece(ctxOf(n, step, 0), mine, peers);
        CHECK(p >= 0);
        if (p < 0) return;               // picker sai: dừng ngay, tránh ghi ra ngoài vector
        CHECK(picked.insert(p).second);  // chưa từng được chọn
        mine[p] = true;
    }
    CHECK_EQ(picked.size(), n);
    CHECK_EQ(picker.pick_piece(ctxOf(n, n, 0), mine, peers), -1);
}

// ---------- Piece đầu tiên chọn ngẫu nhiên ----------

void testFirstPiecesAreRandomNotRarest() {
    // Mảnh 0 là hiếm nhất (1 người). Ở chế độ ngẫu nhiên, mảnh phổ biến cũng phải được chọn.
    Bits mine = {false, false, false};
    Swarm peers = {
        {true, true, true},
        {false, true, true},
        {false, false, true},
    };
    std::set<int> seen;
    for (unsigned seed = 0; seed < 300; ++seed) {
        RarestFirstPicker picker(RarestFirstConfig(), seed);  // mặc định: 4 piece đầu ngẫu nhiên
        const int p = picker.pick_piece(ctxOf(3, 0, 0), mine, peers);
        CHECK(p >= 0 && p <= 2);
        seen.insert(p);
    }
    CHECK_EQ(seen.size(), std::size_t(3));  // đủ cả 3 mảnh, không chỉ riêng mảnh hiếm
}

void testRandomModeStillSkipsOwnedAndUnavailable() {
    Bits mine = {true, false, false, false};
    Swarm peers = {{true, true, false, true}};  // mảnh 2 không ai giữ
    for (unsigned seed = 0; seed < 100; ++seed) {
        RarestFirstPicker picker(RarestFirstConfig(), seed);
        const int p = picker.pick_piece(ctxOf(4, 0, 0), mine, peers);
        CHECK(p == 1 || p == 3);
    }
}

void testSwitchesToRarestFirstAfterThreshold() {
    Bits mine = {false, false, false};
    Swarm peers = {
        {true, true, true},
        {false, true, true},
        {false, false, true},
    };
    for (unsigned seed = 0; seed < 100; ++seed) {
        RarestFirstPicker picker(RarestFirstConfig(), seed);
        // Đã có đúng 4 piece (bằng ngưỡng) => rarest-first: luôn là mảnh 0.
        CHECK_EQ(picker.pick_piece(ctxOf(3, 4, 0), mine, peers), 0);
    }
}

void testRandomFirstCanBeDisabled() {
    Bits mine = {false, false, false};
    Swarm peers = {
        {true, true, true},
        {false, true, true},
        {false, false, true},
    };
    for (unsigned seed = 0; seed < 100; ++seed) {
        RarestFirstPicker picker(noRandomFirst(), seed);
        CHECK_EQ(picker.pick_piece(ctxOf(3, 0, 0), mine, peers), 0);
    }
}

// ---------- Endgame ----------

void testEndgameOnlyWhenNoUnstartedPieceLeft() {
    RarestFirstPicker picker(RarestFirstConfig(), 1);

    CHECK(!picker.in_endgame(ctxOf(10, 3, 2)));   // còn 5 piece chưa bắt đầu
    CHECK(!picker.in_endgame(ctxOf(10, 8, 1)));   // còn 1 piece chưa bắt đầu
    CHECK(picker.in_endgame(ctxOf(10, 8, 2)));    // mọi piece còn thiếu đều đang dở
    CHECK(picker.in_endgame(ctxOf(10, 0, 10)));   // đang dở hết (torrent rất nhỏ)
}

void testNoEndgameWhenFinishedOrEmpty() {
    RarestFirstPicker picker(RarestFirstConfig(), 1);
    CHECK(!picker.in_endgame(ctxOf(10, 10, 0)));  // đã xong
    CHECK(!picker.in_endgame(ctxOf(0, 0, 0)));    // chưa có metadata
}

void testEndgameCanBeDisabled() {
    RarestFirstConfig cfg;
    cfg.endgame_enabled = false;
    cfg.endgame_max_copies = 5;
    RarestFirstPicker picker(cfg, 1);
    CHECK(!picker.in_endgame(ctxOf(10, 8, 2)));
    CHECK_EQ(picker.endgame_max_copies(), std::size_t(1));  // tắt thì không xin trùng
}

// ---------- Tham số cấu hình ----------

void testPipelineAndCopiesFollowConfig() {
    RarestFirstConfig cfg;
    cfg.pipeline_depth = 8;
    cfg.endgame_max_copies = 3;
    RarestFirstPicker picker(cfg, 1);
    CHECK_EQ(picker.pipeline_depth(), std::size_t(8));
    CHECK_EQ(picker.endgame_max_copies(), std::size_t(3));
}

void testConfigValuesAreClamped() {
    RarestFirstConfig cfg;
    cfg.pipeline_depth = 0;       // 0 sẽ làm session không bao giờ xin được block
    cfg.endgame_max_copies = 0;
    RarestFirstPicker picker(cfg, 1);
    CHECK_EQ(picker.pipeline_depth(), std::size_t(1));
    CHECK_EQ(picker.endgame_max_copies(), std::size_t(1));
}

void testDefaultsMatchPlan() {
    RarestFirstPicker picker;  // không tham số: code cũ vẫn biên dịch được
    CHECK_EQ(picker.pipeline_depth(), std::size_t(4));
    CHECK_EQ(picker.endgame_max_copies(), std::size_t(2));
}

// ---------- Tương thích với giao diện cũ ----------

void testBaseClassDefaultsForOldStylePickers() {
    OldStylePicker old;
    PiecePicker& base = old;
    Bits mine = {true, false, false};
    Swarm peers = {Bits(3, true)};

    // pick_piece mặc định chuyển về hàm cũ.
    CHECK_EQ(base.pick_piece(ctxOf(3, 1, 0), mine, peers), 1);
    CHECK(!base.in_endgame(ctxOf(3, 1, 2)));           // mặc định không bao giờ endgame
    CHECK_EQ(base.pipeline_depth(), std::size_t(4));   // khớp giá trị cũ viết cứng trong session
    CHECK_EQ(base.endgame_max_copies(), std::size_t(1));
}

void testUsableThroughBasePointer() {
    RarestFirstPicker concrete(noRandomFirst(), 1);
    PiecePicker* picker = &concrete;
    CHECK_EQ(picker->pick_next_piece(Bits{false, false}, Swarm{{false, true}}), 1);
}

int main() {
    RUN(testRarestFirstPicksRarestPiece);
    RUN(testPickNeverReturnsOwnedOrUnavailablePiece);
    RUN(testReturnsMinusOneWhenNothingToPick);
    RUN(testIgnoresPeerWithWrongSizedBitfield);
    RUN(testTiesAreBrokenRandomlyAndOnlyAmongRarest);
    RUN(testSameSeedGivesSameSequence);
    RUN(testDownloadsEveryPieceExactlyOnce);
    RUN(testFirstPiecesAreRandomNotRarest);
    RUN(testRandomModeStillSkipsOwnedAndUnavailable);
    RUN(testSwitchesToRarestFirstAfterThreshold);
    RUN(testRandomFirstCanBeDisabled);
    RUN(testEndgameOnlyWhenNoUnstartedPieceLeft);
    RUN(testNoEndgameWhenFinishedOrEmpty);
    RUN(testEndgameCanBeDisabled);
    RUN(testPipelineAndCopiesFollowConfig);
    RUN(testConfigValuesAreClamped);
    RUN(testDefaultsMatchPlan);
    RUN(testBaseClassDefaultsForOldStylePickers);
    RUN(testUsableThroughBasePointer);
    return TEST_REPORT();
}
