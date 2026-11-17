// tests/test_metrics.cpp
// Unit test cho Metrics (Người 3).
// Dùng test_framework.h.

#include "test_framework.h"
#include "bench/metrics.h"

#include <chrono>
#include <thread>

using namespace p2p::bench;

void testInitialMetricsAreZero() {
    Metrics m;
    auto snap = m.snapshot();
    CHECK_EQ(snap.bytes_downloaded, std::uint64_t(0));
    CHECK_EQ(snap.bytes_uploaded, std::uint64_t(0));
    CHECK_EQ(snap.pieces_completed, std::uint64_t(0));
    CHECK_EQ(snap.bad_pieces, std::uint64_t(0));
    CHECK_EQ(snap.peer_connections, std::uint64_t(0));
    CHECK(snap.download_rate == 0.0);
    CHECK(snap.upload_rate == 0.0);
}

void testRecordCounters() {
    Metrics m;
    m.record_downloaded(1024);
    m.record_downloaded(2048);
    m.record_uploaded(512);
    m.record_piece_completed();
    m.record_piece_completed();
    m.record_bad_piece();
    m.set_peer_connections(4);

    auto snap = m.snapshot();
    CHECK_EQ(snap.bytes_downloaded, std::uint64_t(3072));
    CHECK_EQ(snap.bytes_uploaded, std::uint64_t(512));
    CHECK_EQ(snap.pieces_completed, std::uint64_t(2));
    CHECK_EQ(snap.bad_pieces, std::uint64_t(1));
    CHECK_EQ(snap.peer_connections, std::uint64_t(4));
}

void testSlidingWindowRate() {
    Metrics m;
    // Ghi nhận 5000 bytes download ngay lúc này
    m.record_downloaded(5000);
    auto snap = m.snapshot();
    // Cửa sổ 5s -> rate = 5000 / 5.0 = 1000.0 B/s
    CHECK(snap.download_rate >= 990.0 && snap.download_rate <= 1010.0);
}

void testResetClearsAll() {
    Metrics m;
    m.record_downloaded(1000);
    m.record_uploaded(2000);
    m.record_piece_completed();
    m.record_bad_piece();
    m.set_peer_connections(3);

    m.reset();
    auto snap = m.snapshot();
    CHECK_EQ(snap.bytes_downloaded, std::uint64_t(0));
    CHECK_EQ(snap.bytes_uploaded, std::uint64_t(0));
    CHECK_EQ(snap.pieces_completed, std::uint64_t(0));
    CHECK_EQ(snap.bad_pieces, std::uint64_t(0));
    CHECK_EQ(snap.peer_connections, std::uint64_t(0));
    CHECK(snap.download_rate == 0.0);
    CHECK(snap.upload_rate == 0.0);
}

int main() {
    RUN(testInitialMetricsAreZero);
    RUN(testRecordCounters);
    RUN(testSlidingWindowRate);
    RUN(testResetClearsAll);
    return TEST_REPORT();
}
