// tests/test_bandwidth_limiter.cpp
// Unit test cho BandwidthLimiter (Người 3).
// Dùng test_framework.h (không dùng gtest).

#include "test_framework.h"
#include "bench/bandwidth_limiter.h"

#include <atomic>
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
// Block 16 KiB với giới hạn 10 KB/s: lớn hơn cả bucket nhưng không được kẹt vĩnh viễn.
void testPacketLargerThanBucketIsEventuallyAllowed() {
    BandwidthLimiter limiter(10000);
    CHECK(limiter.consume(16384));   // bucket đầy -> cho qua và ghi nợ
    CHECK(!limiter.consume(1));      // đang nợ -> phải chờ
    std::this_thread::sleep_for(std::chrono::milliseconds(800));  // trả nợ ~8000 byte, còn dư
    CHECK(limiter.consume(1));
}

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
// consume_both: giới hạn riêng + giới hạn tổng. Chỉ trừ khi CẢ HAI đủ; không rút cạn bucket còn lại khi một bên từ chối.
// ---------------------------------------------------------------------------
void testConsumeBothRequiresBothBuckets() {
    BandwidthLimiter local(1000);
    BandwidthLimiter shared(300);
    CHECK(BandwidthLimiter::consume_both(local, &shared, 300));   // đủ cả hai
    CHECK(!BandwidthLimiter::consume_both(local, &shared, 100));  // giới hạn tổng đã cạn
    // Bucket riêng KHÔNG bị trừ khi lượt bị giới hạn tổng từ chối: còn 700 token.
    CHECK(local.consume(700));
    CHECK(!local.consume(1));
}

void testConsumeBothLocalLimitStillApplies() {
    BandwidthLimiter local(200);
    BandwidthLimiter shared(100000);
    CHECK(BandwidthLimiter::consume_both(local, &shared, 200));
    CHECK(!BandwidthLimiter::consume_both(local, &shared, 50));  // riêng đã cạn dù tổng còn thừa
    CHECK(shared.consume(99800));                                // tổng chỉ bị trừ đúng 200 của lượt thành công
}

void testConsumeBothHandlesUnlimitedNullAndSameObject() {
    BandwidthLimiter unlimitedLocal(0);
    BandwidthLimiter shared(100);
    CHECK(BandwidthLimiter::consume_both(unlimitedLocal, &shared, 100));  // chỉ tổng giới hạn
    CHECK(!BandwidthLimiter::consume_both(unlimitedLocal, &shared, 1));

    BandwidthLimiter local(100);
    CHECK(BandwidthLimiter::consume_both(local, nullptr, 100));  // không có giới hạn tổng
    CHECK(!BandwidthLimiter::consume_both(local, nullptr, 1));

    BandwidthLimiter same(100);
    CHECK(BandwidthLimiter::consume_both(same, &same, 100));  // cùng một đối tượng: không tự khoá chết, không trừ 2 lần
    CHECK(!BandwidthLimiter::consume_both(same, &same, 1));

    BandwidthLimiter none1(0);
    BandwidthLimiter none2(0);
    CHECK(BandwidthLimiter::consume_both(none1, &none2, 1ULL << 40));
}

// Nhiều luồng đảo thứ tự (a,b) / (b,a) không được deadlock; tổng lượng qua không vượt quá bucket tổng.
void testConsumeBothConcurrentNoDeadlockAndBounded() {
    BandwidthLimiter a(1000000);
    BandwidthLimiter b(1000000);
    BandwidthLimiter shared(50000);
    std::atomic<std::uint64_t> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 2000; ++i) {
                BandwidthLimiter& local = (t % 2 == 0) ? a : b;
                if (BandwidthLimiter::consume_both(local, &shared, 100)) granted += 100;
                if (BandwidthLimiter::consume_both(shared, &local, 100)) granted += 100;  // thứ tự khoá ngược
            }
        });
    }
    for (auto& thread : threads) thread.join();
    // Bucket tổng đầy 50000 token + nạp thêm trong lúc chạy (rất ngắn): không thể qua nhiều hơn mức đó đáng kể.
    CHECK(granted.load() >= 50000);
    CHECK(granted.load() <= 50000 + 50000);
}

// ---------------------------------------------------------------------------
int main() {
    RUN(testUnlimitedAlwaysConsumes);
    RUN(testLimitedExceedsBucketReturnsFalse);
    RUN(testPartialConsumes);
    RUN(testRefillAfterDelay);
    RUN(testSetRateDynamic);
    RUN(testSetRateToZeroBecomesUnlimited);
    RUN(testPacketLargerThanBucketIsEventuallyAllowed);
    RUN(testConcurrentConsumeNoCrash);
    RUN(testConsumeBothRequiresBothBuckets);
    RUN(testConsumeBothLocalLimitStillApplies);
    RUN(testConsumeBothHandlesUnlimitedNullAndSameObject);
    RUN(testConsumeBothConcurrentNoDeadlockAndBounded);
    return TEST_REPORT();
}
