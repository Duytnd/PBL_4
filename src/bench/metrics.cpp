// src/bench/metrics.cpp
// Implement class Metrics — sliding-window rate + thread-safe counters.

#include "bench/metrics.h"

#include <algorithm>  // std::remove_if

namespace p2p {
namespace bench {

Metrics::Metrics() : start_time_(std::chrono::steady_clock::now()) {}

// ---------------------------------------------------------------------------
void Metrics::record_downloaded(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_downloaded_ += bytes;
    dl_samples_.push_back({std::chrono::steady_clock::now(), bytes});
}

void Metrics::record_uploaded(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_uploaded_ += bytes;
    ul_samples_.push_back({std::chrono::steady_clock::now(), bytes});
}

void Metrics::record_piece_completed() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++pieces_completed_;
}

void Metrics::record_bad_piece() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++bad_pieces_;
}

void Metrics::set_peer_connections(std::uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_connections_ = count;
}

// ---------------------------------------------------------------------------
// Tính rate bytes/s trong cửa sổ 5 giây gần nhất.
// Phải giữ mutex_ khi gọi hàm này.
// ---------------------------------------------------------------------------
double Metrics::calc_rate(const std::vector<Sample>& samples,
                          std::chrono::steady_clock::time_point now) const {
    if (samples.empty()) return 0.0;

    auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(kWindowSeconds));

    std::uint64_t total_bytes = 0;
    for (const auto& s : samples) {
        if (s.ts >= cutoff) {
            total_bytes += s.bytes;
        }
    }
    return static_cast<double>(total_bytes) / kWindowSeconds;
}

// ---------------------------------------------------------------------------
SessionMetrics Metrics::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    // Dọn sample cũ hơn cửa sổ (tránh vector phình mãi).
    auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(kWindowSeconds));
    dl_samples_.erase(
        std::remove_if(dl_samples_.begin(), dl_samples_.end(),
                       [cutoff](const Sample& s) { return s.ts < cutoff; }),
        dl_samples_.end());
    ul_samples_.erase(
        std::remove_if(ul_samples_.begin(), ul_samples_.end(),
                       [cutoff](const Sample& s) { return s.ts < cutoff; }),
        ul_samples_.end());

    SessionMetrics snap;
    snap.bytes_downloaded = total_downloaded_;
    snap.bytes_uploaded   = total_uploaded_;
    snap.pieces_completed = pieces_completed_;
    snap.bad_pieces       = bad_pieces_;
    snap.peer_connections = peer_connections_;
    snap.start_time       = start_time_;
    snap.download_rate    = calc_rate(dl_samples_, now);
    snap.upload_rate      = calc_rate(ul_samples_, now);
    return snap;
}

// ---------------------------------------------------------------------------
void Metrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_downloaded_  = 0;
    total_uploaded_    = 0;
    pieces_completed_  = 0;
    bad_pieces_        = 0;
    peer_connections_  = 0;
    dl_samples_.clear();
    ul_samples_.clear();
    start_time_ = std::chrono::steady_clock::now();
}

}  // namespace bench
}  // namespace p2p
