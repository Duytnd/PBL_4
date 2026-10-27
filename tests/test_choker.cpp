#include "test_framework.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "strategy/choker.h"

using namespace p2p::strategy;

namespace {

PeerStatus makePeer(const std::string& id, bool interested, double down, double up) {
    return PeerStatus{id, interested, true, down, up};
}

bool contains(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

// 5 peer: D không interested dù rất nhanh.
std::vector<PeerStatus> samplePeers() {
    return {
        makePeer("A", true, 50.0, 10.0),
        makePeer("B", true, 120.0, 15.0),
        makePeer("C", true, 80.0, 12.0),
        makePeer("D", false, 200.0, 20.0),
        makePeer("E", true, 60.0, 5.0),
    };
}

}  // namespace

void testTopThreeByDownloadRateWhenDownloading() {
    TitForTatChoker choker(TitForTatConfig(), 1);
    const auto unchoked = choker.update_chokes(samplePeers(), false);

    // 3 slot chính (B, C, E) + 1 optimistic (chỉ còn A để chọn).
    CHECK_EQ(unchoked.size(), std::size_t(4));
    CHECK(contains(unchoked, "B"));
    CHECK(contains(unchoked, "C"));
    CHECK(contains(unchoked, "E"));
    CHECK(contains(unchoked, "A"));
}

void testNotInterestedPeerNeverUnchoked() {
    for (unsigned seed = 0; seed < 50; ++seed) {
        TitForTatChoker choker(TitForTatConfig(), seed);
        const auto unchoked = choker.update_chokes(samplePeers(), false);
        CHECK(!contains(unchoked, "D"));
    }
}

void testSeedingUsesUploadRate() {
    std::vector<PeerStatus> peers = {
        makePeer("P1", true, 500.0, 10.0),
        makePeer("P2", true, 10.0, 90.0),
        makePeer("P3", true, 10.0, 80.0),
        makePeer("P4", true, 10.0, 70.0),
        makePeer("P5", true, 900.0, 1.0),
    };
    TitForTatChoker choker(TitForTatConfig(), 7);
    const auto unchoked = choker.update_chokes(peers, true);

    // Top 3 theo upload_rate: P2, P3, P4.
    CHECK(contains(unchoked, "P2"));
    CHECK(contains(unchoked, "P3"));
    CHECK(contains(unchoked, "P4"));
    CHECK_EQ(unchoked.size(), std::size_t(4));
}

void testSlotCountNeverExceedsFour() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 20; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 10.0 + i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 3);
    for (int round = 0; round < 10; ++round) {
        const auto unchoked = choker.update_chokes(peers, false);
        CHECK_EQ(unchoked.size(), std::size_t(4));
        // Không trùng peer.
        std::set<std::string> unique(unchoked.begin(), unchoked.end());
        CHECK_EQ(unique.size(), unchoked.size());
    }
}

void testFewerPeersThanSlots() {
    std::vector<PeerStatus> peers = {
        makePeer("X", true, 5.0, 1.0),
        makePeer("Y", true, 9.0, 1.0),
    };
    TitForTatChoker choker(TitForTatConfig(), 3);
    const auto unchoked = choker.update_chokes(peers, false);
    CHECK_EQ(unchoked.size(), std::size_t(2));
    CHECK(choker.current_optimistic_peer().empty());  // không còn ai để chọn optimistic

    CHECK(choker.update_chokes({}, false).empty());
}

void testOptimisticHeldForThreeRoundsThenRotates() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 12; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 100.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 42);

    choker.update_chokes(peers, false);
    const std::string first = choker.current_optimistic_peer();
    CHECK(!first.empty());

    // Vòng 2 và 3: giữ nguyên người cũ.
    choker.update_chokes(peers, false);
    CHECK_EQ(choker.current_optimistic_peer(), first);
    choker.update_chokes(peers, false);
    CHECK_EQ(choker.current_optimistic_peer(), first);

    // Vòng 4: đã đủ 3 vòng, phải đổi sang người khác.
    choker.update_chokes(peers, false);
    CHECK(choker.current_optimistic_peer() != first);
}

void testOptimisticAlwaysInUnchokedList() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 8; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 50.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 5);
    for (int round = 0; round < 9; ++round) {
        const auto unchoked = choker.update_chokes(peers, false);
        CHECK(contains(unchoked, choker.current_optimistic_peer()));
    }
}

void testOptimisticReplacedWhenPeerDisconnects() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 8; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 50.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 11);
    choker.update_chokes(peers, false);
    const std::string held = choker.current_optimistic_peer();

    // Peer optimistic rớt mạng giữa chừng: không được giữ lại.
    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [&](const PeerStatus& p) { return p.peer_id == held; }),
                peers.end());
    const auto unchoked = choker.update_chokes(peers, false);
    CHECK(!contains(unchoked, held));
    CHECK(choker.current_optimistic_peer() != held);
    CHECK(contains(unchoked, choker.current_optimistic_peer()));
}

void testOptimisticDroppedWhenPeerLosesInterest() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 8; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 50.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 13);
    choker.update_chokes(peers, false);
    const std::string held = choker.current_optimistic_peer();

    for (auto& p : peers) {
        if (p.peer_id == held) p.is_interested = false;
    }
    const auto unchoked = choker.update_chokes(peers, false);
    CHECK(!contains(unchoked, held));
}

void testOptimisticMovesAwayWhenPeerBecomesFast() {
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 8; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 50.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 17);
    choker.update_chokes(peers, false);
    const std::string held = choker.current_optimistic_peer();

    // Peer đang giữ slot optimistic bỗng nhanh nhất: giờ nó thuộc top 3 theo tốc độ,
    // không được chiếm thêm slot optimistic nữa (không trùng trong danh sách).
    for (auto& p : peers) {
        if (p.peer_id == held) p.download_rate = 1000.0;
    }
    const auto unchoked = choker.update_chokes(peers, false);
    CHECK_EQ(unchoked.size(), std::size_t(4));
    CHECK(contains(unchoked, held));
    CHECK(choker.current_optimistic_peer() != held);
    std::set<std::string> unique(unchoked.begin(), unchoked.end());
    CHECK_EQ(unique.size(), std::size_t(4));
}

void testTiesAreDeterministicRegardlessOfInputOrder() {
    std::vector<PeerStatus> a = {
        makePeer("P1", true, 10.0, 1.0), makePeer("P2", true, 10.0, 1.0),
        makePeer("P3", true, 10.0, 1.0), makePeer("P4", true, 10.0, 1.0),
        makePeer("P5", true, 10.0, 1.0),
    };
    std::vector<PeerStatus> b(a.rbegin(), a.rend());

    TitForTatConfig cfg;
    TitForTatChoker c1(cfg, 1), c2(cfg, 1);
    const auto r1 = c1.update_chokes(a, false);
    const auto r2 = c2.update_chokes(b, false);

    // 3 slot chính giống nhau (P1, P2, P3 theo thứ tự peer_id).
    CHECK(std::vector<std::string>(r1.begin(), r1.begin() + 3) ==
          std::vector<std::string>(r2.begin(), r2.begin() + 3));
}

void testConfigurableSlotsAndRotation() {
    TitForTatConfig cfg;
    cfg.regular_slots = 2;
    cfg.optimistic_rotation_rounds = 1;

    std::vector<PeerStatus> peers;
    for (int i = 0; i < 10; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 100.0 - i, 1.0));
    }
    TitForTatChoker choker(cfg, 9);

    const auto unchoked = choker.update_chokes(peers, false);
    CHECK_EQ(unchoked.size(), std::size_t(3));  // 2 chính + 1 optimistic

    // Xoay mỗi vòng: vòng sau phải khác vòng trước.
    const std::string first = choker.current_optimistic_peer();
    choker.update_chokes(peers, false);
    CHECK(choker.current_optimistic_peer() != first);
}

void testOptimisticEventuallyCoversAllCandidates() {
    // Không có peer nào bị bỏ đói mãi mãi: qua nhiều vòng, optimistic phải chạm tới mọi ứng viên.
    std::vector<PeerStatus> peers;
    for (int i = 0; i < 7; ++i) {
        peers.push_back(makePeer("P" + std::to_string(i), true, 100.0 - i, 1.0));
    }
    TitForTatChoker choker(TitForTatConfig(), 21);
    std::set<std::string> seen;
    for (int round = 0; round < 400; ++round) {
        choker.update_chokes(peers, false);
        seen.insert(choker.current_optimistic_peer());
    }
    // Ứng viên optimistic là P3..P6 (P0..P2 nằm trong top 3 theo tốc độ).
    CHECK_EQ(seen.size(), std::size_t(4));
}

int main() {
    RUN(testTopThreeByDownloadRateWhenDownloading);
    RUN(testNotInterestedPeerNeverUnchoked);
    RUN(testSeedingUsesUploadRate);
    RUN(testSlotCountNeverExceedsFour);
    RUN(testFewerPeersThanSlots);
    RUN(testOptimisticHeldForThreeRoundsThenRotates);
    RUN(testOptimisticAlwaysInUnchokedList);
    RUN(testOptimisticReplacedWhenPeerDisconnects);
    RUN(testOptimisticDroppedWhenPeerLosesInterest);
    RUN(testOptimisticMovesAwayWhenPeerBecomesFast);
    RUN(testTiesAreDeterministicRegardlessOfInputOrder);
    RUN(testConfigurableSlotsAndRotation);
    RUN(testOptimisticEventuallyCoversAllCandidates);
    return TEST_REPORT();
}
