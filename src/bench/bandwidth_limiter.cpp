// src/bench/bandwidth_limiter.cpp
// Token Bucket Rate Limiter — implement cho BandwidthLimiter.
// Người 3: giới hạn băng thông upload/download theo chiều.

#include "bench/bandwidth_limiter.h"

#include <algorithm>  // std::min

namespace p2p {
namespace bench {

BandwidthLimiter::BandwidthLimiter(std::uint64_t rate_bytes_per_sec)
    : rate_(rate_bytes_per_sec),
      tokens_(static_cast<double>(rate_bytes_per_sec)),
      bucket_max_(rate_bytes_per_sec),
      last_refill_(std::chrono::steady_clock::now()) {}

// ---------------------------------------------------------------------------
// Thay đổi tốc độ giới hạn khi đang chạy (thread-safe).
// ---------------------------------------------------------------------------
void BandwidthLimiter::set_rate(std::uint64_t rate_bytes_per_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_       = rate_bytes_per_sec;
    bucket_max_ = rate_bytes_per_sec;
    // Giới hạn token hiện có không vượt quá bucket mới.
    if (tokens_ > static_cast<double>(bucket_max_)) {
        tokens_ = static_cast<double>(bucket_max_);
    }
    last_refill_ = std::chrono::steady_clock::now();
}

std::uint64_t BandwidthLimiter::rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

// ---------------------------------------------------------------------------
// refill_internal() — caller phải giữ mutex_.
// ---------------------------------------------------------------------------
void BandwidthLimiter::refill_internal() {
    if (rate_ == 0) return;

    auto now     = std::chrono::steady_clock::now();
    double delta = std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;

    double added = static_cast<double>(rate_) * delta;
    tokens_ = std::min(tokens_ + added, static_cast<double>(bucket_max_));
}

// ---------------------------------------------------------------------------
// refill() — public, thread-safe.
// Nạp token theo thời gian đã trôi qua. Thường không cần gọi tay vì
// consume() tự nạp trước khi kiểm tra; dùng khi muốn "warm up" bucket
// sau thời gian dài idle.
// ---------------------------------------------------------------------------
void BandwidthLimiter::refill() {
    std::lock_guard<std::mutex> lock(mutex_);
    refill_internal();
}

// ---------------------------------------------------------------------------
// Tiêu bytes token. Trả true nếu đủ token (hoặc rate=0).
// ---------------------------------------------------------------------------
bool BandwidthLimiter::consume(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // rate == 0 → không giới hạn
    if (rate_ == 0) return true;

    refill_internal();

    // Gói lớn hơn cả bucket (vd. block 16 KiB với giới hạn 10 KB/s) sẽ không bao giờ đủ token nếu
    // đòi đủ `bytes`. Chỉ cần bucket đầy là cho qua và ghi nợ (tokens_ âm): các lần sau phải chờ
    // trả hết nợ, nên tốc độ trung bình vẫn đúng bằng rate_.
    const double needed = static_cast<double>(bytes);
    const double threshold = std::min(needed, static_cast<double>(bucket_max_));
    if (tokens_ >= threshold) {
        tokens_ -= needed;
        return true;
    }
    return false;
}

}  // namespace bench
}  // namespace p2p
