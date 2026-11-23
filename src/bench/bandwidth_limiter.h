#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace p2p {
namespace bench {

// BandwidthLimiter — Token Bucket Rate Limiter.
//
// Mỗi chiều (download / upload) tạo một instance riêng.
// Khi session muốn gửi/nhận N bytes, gọi consume(N):
//   - Nếu đủ token thì tiêu ngay và trả về true.
//   - Nếu không đủ thì trả về false (bên trên tự quyết định chờ hoặc bỏ qua).
//
// Thread-safe: safe khi nhiều luồng gọi consume() cùng lúc.
//
// Cách dùng:
//   BandwidthLimiter limiter(512 * 1024);  // giới hạn 512 KB/s
//   if (limiter.consume(bytes_to_send)) {
//       socket.write(buf, bytes_to_send);
//   }
//
// 0 = không giới hạn (consume() luôn trả về true).
class BandwidthLimiter {
public:
    // rate_bytes_per_sec: số byte/s tối đa. 0 = vô hạn.
    explicit BandwidthLimiter(std::uint64_t rate_bytes_per_sec = 0);

    // Thay đổi giới hạn tốc độ trong khi chạy.
    void set_rate(std::uint64_t rate_bytes_per_sec);

    // Lấy giới hạn tốc độ hiện tại (0 = vô hạn).
    std::uint64_t rate() const;

    // Cố gắng tiêu bytes token.
    // Trả true nếu bucket còn đủ, false nếu vượt giới hạn.
    // Nếu rate = 0 thì luôn trả true.
    bool consume(std::uint64_t bytes);

    // Làm đầy token theo thời gian thực tế đã trôi qua.
    // Thường được gọi bởi chính consume() nên người dùng không cần gọi tay.
    // Có thể gọi tường minh nếu muốn reset bucket sau khi idle.
    void refill();

    // Tiêu `bytes` đồng thời từ `local` và `shared` (có thể null / cùng một đối tượng). Chỉ trừ khi CẢ HAI cùng đủ:
    // nếu không, bucket nào còn token vẫn giữ nguyên, nên một giới hạn tổng đang cạn không rút cạn giới hạn riêng của
    // từng torrent (và ngược lại).
    static bool consume_both(BandwidthLimiter& local, BandwidthLimiter* shared, std::uint64_t bytes);

private:
    // Caller giữ mutex_. can_pay: bucket có đủ cho gói này không (đã refill); pay: trừ token.
    bool can_pay_locked(std::uint64_t bytes);
    void pay_locked(std::uint64_t bytes);

    void refill_internal();  // Hàm nội bộ — caller phải giữ mutex_

    mutable std::mutex mutex_;

    std::uint64_t rate_;                            // bytes/s (0 = unlimited)
    double        tokens_;                          // token hiện có (byte)
    std::uint64_t bucket_max_;                      // token tối đa = rate_ (burst = 1s)
    std::chrono::steady_clock::time_point last_refill_;
};

}  // namespace bench
}  // namespace p2p

