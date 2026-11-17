// tests/test_peer_policy.cpp
// Unit test cho PeerPolicy (Người 3).
// Dùng test_framework.h (không dùng gtest).

#include "test_framework.h"
#include "strategy/peer_policy.h"

#include <string>

using namespace p2p::strategy;

// Địa chỉ giả để test
static const std::string kAddr1 = "192.168.1.10:6881";
static const std::string kAddr2 = "192.168.1.20:6881";
static const std::string kAddr3 = "10.0.0.1:6881";

// ---------------------------------------------------------------------------
// Ban peer sau khi vượt ngưỡng bad_piece_threshold
// ---------------------------------------------------------------------------
void testBanAfterThreshold() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 3;
    PeerPolicy policy(cfg);

    CHECK(!policy.is_banned(kAddr1));
    // Report 2 lần — chưa đủ
    CHECK(!policy.report_bad_piece(kAddr1));  // 1
    CHECK(!policy.report_bad_piece(kAddr1));  // 2
    CHECK(!policy.is_banned(kAddr1));
    // Report lần 3 → bị ban
    CHECK( policy.report_bad_piece(kAddr1));  // 3 → banned
    CHECK( policy.is_banned(kAddr1));
}

// ---------------------------------------------------------------------------
// Dưới ngưỡng → chưa bị ban
// ---------------------------------------------------------------------------
void testUnderThresholdNotBanned() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 5;
    PeerPolicy policy(cfg);

    for (int i = 0; i < 4; ++i) {
        policy.report_bad_piece(kAddr2);
    }
    CHECK(!policy.is_banned(kAddr2));
}

// ---------------------------------------------------------------------------
// at_connection_limit: n >= max → true
// ---------------------------------------------------------------------------
void testAtConnectionLimit() {
    PeerPolicyConfig cfg;
    cfg.max_connections = 10;
    PeerPolicy policy(cfg);

    CHECK(!policy.at_connection_limit(9));
    CHECK( policy.at_connection_limit(10));
    CHECK( policy.at_connection_limit(100));
}

// ---------------------------------------------------------------------------
// clear_bans() xoá sạch danh sách ban
// ---------------------------------------------------------------------------
void testClearBans() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 2;
    PeerPolicy policy(cfg);

    policy.report_bad_piece(kAddr1);
    policy.report_bad_piece(kAddr1);  // → banned
    CHECK( policy.is_banned(kAddr1));

    policy.clear_bans();
    CHECK(!policy.is_banned(kAddr1));
}

// ---------------------------------------------------------------------------
// Peer đã bị ban rồi, report thêm → vẫn là banned (không double-ban)
// ---------------------------------------------------------------------------
void testAlreadyBannedReportNoDoubleCount() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 2;
    PeerPolicy policy(cfg);

    policy.report_bad_piece(kAddr3);
    policy.report_bad_piece(kAddr3);  // → banned now

    // Lần report tiếp theo không thay đổi trạng thái (đã ban rồi)
    bool second_ban = policy.report_bad_piece(kAddr3);
    // second_ban == false (đã bị ban rồi, không phải lần đầu bị ban)
    CHECK(!second_ban);
    CHECK( policy.is_banned(kAddr3));
}

// ---------------------------------------------------------------------------
// banned_addresses() trả đúng danh sách
// ---------------------------------------------------------------------------
void testBannedAddressesList() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 1;
    PeerPolicy policy(cfg);

    policy.report_bad_piece(kAddr1);  // → banned
    policy.report_bad_piece(kAddr2);  // → banned

    auto banned = policy.banned_addresses();
    bool has1 = false, has2 = false;
    for (const auto& a : banned) {
        if (a == kAddr1) has1 = true;
        if (a == kAddr2) has2 = true;
    }
    CHECK(has1);
    CHECK(has2);
    CHECK_EQ(static_cast<int>(banned.size()), 2);
}

// ---------------------------------------------------------------------------
// Ngưỡng = 0 → ban ngay từ lần report đầu tiên (count >= 0 ngay sau ++count)
// ---------------------------------------------------------------------------
void testZeroThresholdBansImmediately() {
    PeerPolicyConfig cfg;
    cfg.bad_piece_threshold = 0;  // 0 → count(1) >= 0 → ban ngay lần đầu
    PeerPolicy policy(cfg);

    // Lần report đầu tiên sẽ ban ngay (count=1 >= threshold=0)
    bool banned_now = policy.report_bad_piece(kAddr1);
    CHECK(banned_now);
    CHECK( policy.is_banned(kAddr1));
}

// ---------------------------------------------------------------------------
int main() {
    RUN(testBanAfterThreshold);
    RUN(testUnderThresholdNotBanned);
    RUN(testAtConnectionLimit);
    RUN(testClearBans);
    RUN(testAlreadyBannedReportNoDoubleCount);
    RUN(testBannedAddressesList);
    RUN(testZeroThresholdBansImmediately);
    return TEST_REPORT();
}
