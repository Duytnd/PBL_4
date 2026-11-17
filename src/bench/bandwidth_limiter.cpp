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
}

std::uint64_t BandwidthLimiter::rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

// ---------------------------------------------------------------------------
// Nạp token theo thời gian thực tế đã trôi qua (phải giữ lock trước khi gọi).
// ---------------------------------------------------------------------------
void BandwidthLimiter::refill() {
    // Hàm này được gọi NỘI BỘ từ consume() — lock đã được giữ bởi caller.
    // Nếu gọi từ bên ngoài, tự lock:
    // (Phiên bản public bên dưới sẽ tự lock.)
    auto now     = std::chrono::steady_clock::now();
    double delta = std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;

    if (rate_ == 0) return;  // unlimited — không cần nạp

    double added = static_cast<double>(rate_) * delta;
    tokens_ = std::min(tokens_ + added, static_cast<double>(bucket_max_));
}

// ---------------------------------------------------------------------------
// Tiêu bytes token. Trả true nếu đủ token (hoặc rate=0).
// ---------------------------------------------------------------------------
bool BandwidthLimiter::consume(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // rate == 0 → không giới hạn
    if (rate_ == 0) return true;

    // Nạp token theo thời gian trước khi kiểm tra
    {
        auto now     = std::chrono::steady_clock::now();
        double delta = std::chrono::duration<double>(now - last_refill_).count();
        last_refill_ = now;
        double added = static_cast<double>(rate_) * delta;
        tokens_ = std::min(tokens_ + added, static_cast<double>(bucket_max_));
    }

    double needed = static_cast<double>(bytes);
    if (tokens_ >= needed) {
        tokens_ -= needed;
        return true;
    }
    return false;
}

}  // namespace bench
}  // namespace p2p
