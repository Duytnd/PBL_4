#pragma once
// src/bench/metrics.h
// Bộ ghi số liệu hiệu năng (performance metrics) cho một session P2P.
// Người 3: đo lường, so sánh chiến lược piece-picking, choker, bandwidth.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace p2p {
namespace bench {

// ---------------------------------------------------------------------------
// Snapshot bất biến — an toàn để đọc mà không cần lock.
// ---------------------------------------------------------------------------
struct SessionMetrics {
    std::uint64_t bytes_downloaded  = 0;  // tổng byte đã nhận
    std::uint64_t bytes_uploaded    = 0;  // tổng byte đã gửi
    double        download_rate     = 0.0;  // bytes/s, sliding window 5 s
    double        upload_rate       = 0.0;
    std::uint64_t pieces_completed  = 0;  // piece đã xác nhận SHA-1 đúng
    std::uint64_t bad_pieces        = 0;  // piece sai SHA-1 (bị loại)
    std::uint64_t peer_connections  = 0;  // số kết nối peer hiện tại
    std::chrono::steady_clock::time_point start_time;
};

// ---------------------------------------------------------------------------
// Metrics — thread-safe, sliding-window rate.
//
// Cách dùng:
//   Metrics m;
//   m.record_downloaded(1024);
//   auto snap = m.snapshot();
//   std::cout << snap.download_rate << " B/s\n";
// ---------------------------------------------------------------------------
class Metrics {
public:
    explicit Metrics();

    // Ghi nhận byte đã download / upload.
    void record_downloaded(std::uint64_t bytes);
    void record_uploaded(std::uint64_t bytes);

    // Ghi nhận piece hoàn thành (SHA-1 đúng).
    void record_piece_completed();

    // Ghi nhận piece sai SHA-1.
    void record_bad_piece();

    // Cập nhật số kết nối peer hiện tại.
    void set_peer_connections(std::uint64_t count);

    // Lấy snapshot tại thời điểm hiện tại (thread-safe).
    // Rate được tính bằng sliding window 5 giây.
    SessionMetrics snapshot() const;

    // Reset toàn bộ số liệu (dùng khi bắt đầu session mới).
    void reset();

private:
    // Sliding window cho tốc độ: lưu (time, bytes) trong 5 s gần nhất.
    struct Sample {
        std::chrono::steady_clock::time_point ts;
        std::uint64_t bytes;
    };

    // Tính sliding-window rate — phải giữ mutex_ khi gọi.
    double calc_rate(const std::vector<Sample>& samples,
                     std::chrono::steady_clock::time_point now) const;

    mutable std::mutex mutex_;

    std::uint64_t total_downloaded_  = 0;
    std::uint64_t total_uploaded_    = 0;
    std::uint64_t pieces_completed_  = 0;
    std::uint64_t bad_pieces_        = 0;
    std::uint64_t peer_connections_  = 0;

    mutable std::vector<Sample> dl_samples_;   // sliding window download
    mutable std::vector<Sample> ul_samples_;   // sliding window upload

    std::chrono::steady_clock::time_point start_time_;

    static constexpr double kWindowSeconds = 5.0;
};

}  // namespace bench
}  // namespace p2p
