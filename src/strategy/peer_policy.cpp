#include "strategy/peer_policy.h"

#include <algorithm>

namespace p2p {
namespace strategy {

PeerPolicy::PeerPolicy(PeerPolicyConfig config)
    : config_(config) {}

bool PeerPolicy::is_banned(const std::string& address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return banned_.count(address) > 0;
}

bool PeerPolicy::at_connection_limit(std::size_t current_connections) const {
    return current_connections >= config_.max_connections;
}

bool PeerPolicy::report_bad_piece(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (banned_.count(address)) return false;  // đã ban rồi, không cần xử lý lại

    int& count = strikes_[address];
    ++count;
    if (count >= config_.bad_piece_threshold) {
        banned_.insert(address);
        return true;
    }
    return false;
}

std::vector<std::string> PeerPolicy::banned_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(banned_.begin(), banned_.end());
}

int PeerPolicy::bad_piece_count(const std::string& address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = strikes_.find(address);
    return it != strikes_.end() ? it->second : 0;
}

void PeerPolicy::clear_bans() {
    std::lock_guard<std::mutex> lock(mutex_);
    banned_.clear();
    strikes_.clear();
}

}  // namespace strategy
}  // namespace p2p

