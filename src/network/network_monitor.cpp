#include "network/network_monitor.h"

#include <algorithm>
#include <utility>

#include "network/lan_address.h"

namespace p2p::network {

NetworkSnapshot NetworkMonitor::probe_system() {
    NetworkSnapshot snapshot;
    snapshot.addresses = local_ipv4_addresses(16);
    std::sort(snapshot.addresses.begin(), snapshot.addresses.end());
    const std::string primary = detect_lan_ip();
    // detect_lan_ip trả 127.0.0.1 khi không có card mạng dùng được: coi là mất mạng.
    if (!primary.empty() && !is_loopback_address(primary)) {
        snapshot.primary_ip = primary;
    }
    return snapshot;
}

NetworkMonitor& NetworkMonitor::instance() {
    static NetworkMonitor monitor;
    return monitor;
}

NetworkMonitor::NetworkMonitor(Probe probe, std::chrono::milliseconds interval, int confirm_polls)
    : probe_(probe ? std::move(probe) : Probe(&NetworkMonitor::probe_system)),
      interval_ms_(interval.count() > 0 ? interval.count() : 1000),
      confirm_polls_(std::max(1, confirm_polls)) {}

void NetworkMonitor::configure_for_testing(Probe probe, std::chrono::milliseconds interval, int confirm_polls) {
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        probe_ = probe ? std::move(probe) : Probe(&NetworkMonitor::probe_system);
    }
    interval_ms_ = interval.count() > 0 ? interval.count() : 1000;
    confirm_polls_ = std::max(1, confirm_polls);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        initialized_ = false;  // lượt dò kế tiếp đặt lại mốc
    }
    wake_cv_.notify_all();  // luồng nền đang ngủ theo chu kỳ cũ thì dậy sớm để áp chu kỳ mới
}

NetworkMonitor::~NetworkMonitor() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        stop_requested_ = true;
    }
    wake_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::uint64_t NetworkMonitor::subscribe(Callback callback) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        id = next_id_++;
        subscribers_[id] = std::make_shared<Callback>(std::move(callback));
    }
    if (!thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            stop_requested_ = false;
        }
        thread_ = std::thread(&NetworkMonitor::run, this);
    }
    return id;
}

void NetworkMonitor::unsubscribe(std::uint64_t id) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        subscribers_.erase(id);
        empty = subscribers_.empty();
    }
    // Chờ callback đang chạy (nếu có) xong: sau điểm này callback của `id` không bao giờ được gọi nữa.
    { std::lock_guard<std::mutex> wait_for_running_callback(callback_mutex_); }

    if (empty && thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            stop_requested_ = true;
        }
        wake_cv_.notify_all();
        thread_.join();
    }
}

NetworkSnapshot NetworkMonitor::run_probe() {
    Probe probe;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        probe = probe_;
    }
    return probe();
}

void NetworkMonitor::run() {
    // Đặt mốc ngay khi luồng chạy để thay đổi đầu tiên sau đó được nhận ra.
    try {
        poll_once();
    } catch (...) {
    }
    std::unique_lock<std::mutex> lock(wake_mutex_);
    while (!stop_requested_) {
        if (wake_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms_.load()),
                              [this]() { return stop_requested_; })) {
            break;
        }
        lock.unlock();
        try {
            poll_once();
        } catch (...) {
            // Lỗi dò (hiếm) không được làm sập luồng nền.
        }
        lock.lock();
    }
}

void NetworkMonitor::poll_once() {
    const NetworkSnapshot snapshot = run_probe();  // dò ngoài khoá state: có thể chậm
    const int confirm_polls = confirm_polls_.load();

    NetworkChange change;
    bool fire = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) {
            initialized_ = true;
            stable_ = candidate_ = snapshot;
            candidate_count_ = confirm_polls;
            if (snapshot.online()) {
                last_online_ = snapshot;
            }
            return;
        }

        if (snapshot == candidate_) {
            if (candidate_count_ < confirm_polls) {
                ++candidate_count_;
            }
        } else {
            candidate_ = snapshot;
            candidate_count_ = 1;
        }
        if (candidate_count_ < confirm_polls || candidate_ == stable_) {
            return;
        }

        // Trạng thái mới đã ổn định.
        if (!candidate_.online()) {
            stable_ = candidate_;  // mất mạng: chỉ ghi nhận, last_online_ giữ nguyên; chờ mạng quay lại mới báo
            return;
        }
        const bool was_offline = !stable_.online();
        change.before = was_offline ? last_online_ : stable_;
        change.after = candidate_;
        change.was_offline = was_offline;
        change.primary_changed = change.before.primary_ip != change.after.primary_ip;
        stable_ = candidate_;
        last_online_ = candidate_;
        fire = true;
    }
    if (fire) {
        notify(change);
    }
}

void NetworkMonitor::notify(const NetworkChange& change) {
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Callback>>> targets;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        for (const auto& [id, callback] : subscribers_) {
            targets.emplace_back(id, callback);
        }
    }
    std::lock_guard<std::mutex> running(callback_mutex_);
    for (const auto& [id, callback] : targets) {
        {
            // Có thể đã huỷ đăng ký trong lúc chờ khoá: bỏ qua, không gọi vào đối tượng sắp bị huỷ.
            std::lock_guard<std::mutex> lock(subs_mutex_);
            if (subscribers_.find(id) == subscribers_.end()) {
                continue;
            }
        }
        try {
            if (callback && *callback) {
                (*callback)(change);
            }
        } catch (...) {
            // Callback lỗi không được chặn các callback khác.
        }
    }
}

std::string NetworkMonitor::primary_ip() {
    return current().primary_ip;
}

NetworkSnapshot NetworkMonitor::current() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (initialized_) {
            return last_online_.online() ? last_online_ : stable_;
        }
    }
    const NetworkSnapshot snapshot = run_probe();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
        initialized_ = true;
        stable_ = candidate_ = snapshot;
        candidate_count_ = confirm_polls_.load();
        if (snapshot.online()) {
            last_online_ = snapshot;
        }
    }
    return last_online_.online() ? last_online_ : stable_;
}

}  // namespace p2p::network
