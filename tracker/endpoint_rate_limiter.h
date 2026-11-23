#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace p2p {

// Giới hạn tần suất theo khoá (vd. "ip:port" nguồn), BỘ NHỚ BỊ CHẶN.
//
// Bản cũ trong tracker/main.cpp là một unordered_map chỉ thêm không xoá: kẻ gửi gói UDP từ nhiều nguồn
// (hoặc giả nguồn) làm map phình vô hạn cho tới hết RAM. Ở đây:
//  - allow(key) chỉ cho qua nếu khoá đó chưa được cho qua trong `minInterval` gần nhất;
//  - khi đầy `maxEntries`, các mục quá cũ (hơn `staleAfter`) bị dọn;
//  - vẫn đầy thì TỪ CHỐI khoá mới (fail-closed): tracker không bị kéo vào xử lý, và RAM không tăng thêm.
class EndpointRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    explicit EndpointRateLimiter(
        std::chrono::milliseconds minInterval = std::chrono::milliseconds(25),
        std::size_t maxEntries = 16384,
        std::chrono::milliseconds staleAfter = std::chrono::milliseconds(2000))
        : minInterval_(minInterval), maxEntries_(maxEntries), staleAfter_(staleAfter) {}

    bool allow(const std::string& key, Clock::time_point now = Clock::now()) {
        const auto it = lastSeen_.find(key);
        if (it != lastSeen_.end()) {
            if (now - it->second < minInterval_) {
                return false;
            }
            it->second = now;
            return true;
        }
        if (lastSeen_.size() >= maxEntries_) {
            purgeStale(now);
            if (lastSeen_.size() >= maxEntries_) {
                return false;
            }
        }
        lastSeen_.emplace(key, now);
        return true;
    }

    [[nodiscard]] std::size_t size() const { return lastSeen_.size(); }

private:
    void purgeStale(Clock::time_point now) {
        for (auto it = lastSeen_.begin(); it != lastSeen_.end();) {
            if (now - it->second > staleAfter_) {
                it = lastSeen_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::chrono::milliseconds minInterval_;
    std::size_t maxEntries_;
    std::chrono::milliseconds staleAfter_;
    std::unordered_map<std::string, Clock::time_point> lastSeen_;
};

}  // namespace p2p
