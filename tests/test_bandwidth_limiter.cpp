// tests/test_bandwidth_limiter.cpp
// Unit test cho BandwidthLimiter (Người 3).
// Dùng test_framework.h (không dùng gtest).

#include "test_framework.h"
#include "bench/bandwidth_limiter.h"

#include <chrono>
#include <thread>
#include <vector>

using namespace p2p::bench;

// ---------------------------------------------------------------------------
// rate = 0 → unlimited, luôn trả true
// ---------------------------------------------------------------------------
void testUnlimitedAlwaysConsumes() {
    BandwidthLimiter limiter(0);
    CHECK( limiter.consume(1));
    CHECK( limiter.consume(1024 * 1024));       // 1 MB
    CHECK( limiter.consume(1024ULL * 1024 * 1024)); // 1 GB
}

// ---------------------------------------------------------------------------
// Bucket ban đầu đầy = rate. Tiêu hơn bucket → false.
// ---------------------------------------------------------------------------
void testLimitedExceedsBucketReturnsFalse() {
    BandwidthLimiter limiter(1000);  // 1000 bytes/s, bucket = 1000
    // Tiêu đúng bucket → true
    CHECK( limiter.consume(1000));
    // Tiêu thêm (bucket trống) → false
    CHECK(!limiter.consume(1));
}

// ---------------------------------------------------------------------------
// Tiêu từng phần — tích lũy cho đến khi hết bucket
// ---------------------------------------------------------------------------
void testPartialConsumes() {
    BandwidthLimiter limiter(500);  // bucket = 500
    CHECK( limiter.consume(100));
    CHECK( limiter.consume(100));
    CHECK( limiter.consume(100));
    CHECK( limiter.consume(100));
    CHECK( limiter.consume(100));   // 5 × 100 = 500 → bucket hết
    CHECK(!limiter.consume(1));     // hết → false
}

// ---------------------------------------------------------------------------
// Sau khoảng 1 giây, bucket nạp lại (gần đủ rate bytes)
// ---------------------------------------------------------------------------
void testRefillAfterDelay() {
    BandwidthLimiter limiter(1000);  // 1000 bytes/s
    CHECK( limiter.consume(1000));   // làm cạn bucket
    CHECK(!limiter.consume(500));    // hết → false

    // Chờ ~1 giây để bucket nạp lại ~1000 bytes
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));

    // Giờ nên có đủ ~1000 tokens
    CHECK( limiter.consume(900));    // nên pass
}

// ---------------------------------------------------------------------------
// set_rate() thay đổi giới hạn khi đang chạy
// ---------------------------------------------------------------------------
void testSetRateDynamic() {
    BandwidthLimiter limiter(500);  // ban đầu 500 B/s
    CHECK_EQ(limiter.rate(), std::uint64_t(500));

    limiter.set_rate(2000);
    CHECK_EQ(limiter.rate(), std::uint64_t(2000));

    // Bucket mới = 2000, nhưng token hiện tại bị cap tại 500 (bucket cũ còn).
    // Sau set_rate, bucket_max = 2000 nhưng tokens_ không tự tăng ngay.
    // Phải chờ refill hoặc tiêu dần rồi mới thấy rate mới.
}

// ---------------------------------------------------------------------------
// set_rate(0) → chuyển sang unlimited
// ---------------------------------------------------------------------------
void testSetRateToZeroBecomesUnlimited() {
    BandwidthLimiter limiter(100);
    limiter.consume(100);            // làm cạn
    CHECK(!limiter.consume(1));

    limiter.set_rate(0);             // unlimited
    CHECK( limiter.consume(999999));
}

// ---------------------------------------------------------------------------
// Nhiều luồng consume() không crash / race condition (smoke test)
// ---------------------------------------------------------------------------
void testConcurrentConsumeNoCrash() {
    BandwidthLimiter limiter(0);  // unlimited để tránh tính đúng sai
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&limiter]() {
            for (int j = 0; j < 1000; ++j) {
                limiter.consume(64);
            }
        });
    }
    for (auto& t : threads) t.join();
    // Không crash là pass
    CHECK(true);
}

// ---------------------------------------------------------------------------
int main() {
    RUN(testUnlimitedAlwaysConsumes);
    RUN(testLimitedExceedsBucketReturnsFalse);
    RUN(testPartialConsumes);
    RUN(testRefillAfterDelay);
    RUN(testSetRateDynamic);
    RUN(testSetRateToZeroBecomesUnlimited);
    RUN(testConcurrentConsumeNoCrash);
    return TEST_REPORT();
}
