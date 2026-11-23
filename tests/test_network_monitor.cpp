// tests/test_network_monitor.cpp
// Unit test cho NetworkMonitor: phát hiện đổi mạng, chống nhiễu, mất mạng/có lại, huỷ đăng ký an toàn.
// Dùng probe giả (không đụng card mạng thật) trừ test cuối kiểm tra probe thật không trả giá trị vô lý.

#include "test_framework.h"
#include "network/network_monitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace p2p::network;
using namespace std::chrono_literals;

namespace {

NetworkSnapshot snap(const std::string& primary, std::vector<std::string> addresses = {}) {
    NetworkSnapshot s;
    s.primary_ip = primary;
    s.addresses = std::move(addresses);
    if (s.addresses.empty() && !primary.empty()) {
        s.addresses.push_back(primary);
    }
    return s;
}

// Probe giả: test đổi `current` rồi gọi poll_once().
struct FakeNet {
    NetworkSnapshot current;
    NetworkMonitor::Probe probe() {
        return [this]() { return current; };
    }
};

}  // namespace

// Lần dò đầu chỉ đặt mốc; không đổi gì thì không có sự kiện.
void testBaselineAndNoChangeFiresNothing() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 1h, 2);
    monitor.poll_once();
    monitor.poll_once();
    monitor.poll_once();
    CHECK_EQ(monitor.primary_ip(), std::string("192.168.1.5"));
}

// Đổi IP chính phải được xác nhận 2 lần dò liên tiếp mới tính; lần dò đầu thấy khác thì chưa báo.
void testChangeNeedsConfirmationAndReportsBeforeAfter() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 2);
    std::mutex m;
    std::vector<NetworkChange> events;
    const auto id = monitor.subscribe([&](const NetworkChange& c) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(c);
    });
    std::this_thread::sleep_for(60ms);  // luồng nền đặt mốc
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(events.empty());
    }
    net.current = snap("10.0.0.7");
    // chờ tối đa 2 giây
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!events.empty()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    monitor.unsubscribe(id);
    std::lock_guard<std::mutex> lock(m);
    CHECK_EQ(events.size(), std::size_t(1));
    if (!events.empty()) {
        CHECK_EQ(events[0].before.primary_ip, std::string("192.168.1.5"));
        CHECK_EQ(events[0].after.primary_ip, std::string("10.0.0.7"));
        CHECK(events[0].primary_changed);
        CHECK(!events[0].was_offline);
    }
    CHECK_EQ(monitor.primary_ip(), std::string("10.0.0.7"));
}

// Nhấp nháy 1 lần dò (A -> B -> A) không được báo: đó là trạng thái dở dang lúc hệ điều hành chuyển mạng.
void testSingleFlapIsIgnored() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 1h, 2);
    monitor.poll_once();                 // mốc
    net.current = snap("169.254.9.9");   // trạng thái dở dang
    monitor.poll_once();                 // 1 lần thấy: chưa tính
    net.current = snap("192.168.1.5");   // trở lại
    monitor.poll_once();
    monitor.poll_once();
    CHECK_EQ(monitor.primary_ip(), std::string("192.168.1.5"));
}

// Mất mạng không phát sự kiện; có lại (cùng IP hay khác IP) phát đúng một sự kiện với was_offline = true.
void testOfflineThenBackReportsOnce() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 2);
    std::mutex m;
    std::vector<NetworkChange> events;
    const auto id = monitor.subscribe([&](const NetworkChange& c) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(c);
    });
    std::this_thread::sleep_for(60ms);

    net.current = snap("");  // mất mạng
    std::this_thread::sleep_for(150ms);
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(events.empty());
    }
    // IP cuối cùng còn online vẫn đọc được trong lúc mất mạng.
    CHECK_EQ(monitor.primary_ip(), std::string("192.168.1.5"));

    net.current = snap("192.168.1.5");  // có lại, CÙNG IP (router khởi động lại)
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!events.empty()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(60ms);  // không được có sự kiện thứ hai
    monitor.unsubscribe(id);
    std::lock_guard<std::mutex> lock(m);
    CHECK_EQ(events.size(), std::size_t(1));
    if (!events.empty()) {
        CHECK(events[0].was_offline);
        CHECK(!events[0].primary_changed);
        CHECK_EQ(events[0].before.primary_ip, std::string("192.168.1.5"));
    }
}

// Mất mạng ở đây rồi có lại ở IP khác: primary_changed + was_offline cùng true, before là IP online gần nhất.
void testOfflineThenDifferentNetwork() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 2);
    std::mutex m;
    std::vector<NetworkChange> events;
    const auto id = monitor.subscribe([&](const NetworkChange& c) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(c);
    });
    std::this_thread::sleep_for(60ms);
    net.current = snap("");
    std::this_thread::sleep_for(100ms);
    net.current = snap("172.20.10.3");
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!events.empty()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    monitor.unsubscribe(id);
    std::lock_guard<std::mutex> lock(m);
    CHECK_EQ(events.size(), std::size_t(1));
    if (!events.empty()) {
        CHECK(events[0].was_offline);
        CHECK(events[0].primary_changed);
        CHECK_EQ(events[0].before.primary_ip, std::string("192.168.1.5"));
        CHECK_EQ(events[0].after.primary_ip, std::string("172.20.10.3"));
    }
}

// Chỉ thêm một card phụ (VPN bật): có sự kiện nhưng primary_changed = false, was_offline = false.
void testSecondaryAdapterOnlyIsNotPrimaryChange() {
    FakeNet net{snap("192.168.1.5", {"192.168.1.5"})};
    NetworkMonitor monitor(net.probe(), 5ms, 2);
    std::mutex m;
    std::vector<NetworkChange> events;
    const auto id = monitor.subscribe([&](const NetworkChange& c) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(c);
    });
    std::this_thread::sleep_for(60ms);
    net.current = snap("192.168.1.5", {"10.8.0.2", "192.168.1.5"});
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!events.empty()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    monitor.unsubscribe(id);
    std::lock_guard<std::mutex> lock(m);
    CHECK_EQ(events.size(), std::size_t(1));
    if (!events.empty()) {
        CHECK(!events[0].primary_changed);
        CHECK(!events[0].was_offline);
    }
}

// Sau unsubscribe không còn callback nào; unsubscribe chờ callback đang chạy (đối tượng đăng ký huỷ được ngay sau đó).
void testUnsubscribeWaitsForRunningCallback() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 1);
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::atomic<int> calls{0};
    const auto id = monitor.subscribe([&](const NetworkChange&) {
        ++calls;
        entered = true;
        std::this_thread::sleep_for(250ms);
        finished = true;
    });
    std::this_thread::sleep_for(60ms);
    net.current = snap("10.1.1.1");
    for (int i = 0; i < 200 && !entered.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(entered.load());
    monitor.unsubscribe(id);
    CHECK(finished.load());  // unsubscribe không trả về khi callback chưa xong

    const int calls_after_unsubscribe = calls.load();
    net.current = snap("10.2.2.2");
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(calls.load(), calls_after_unsubscribe);
}

// Một callback ném ngoại lệ không chặn callback khác và không làm sập luồng nền.
void testThrowingCallbackDoesNotBreakOthers() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 1);
    std::atomic<int> good_calls{0};
    const auto bad = monitor.subscribe([](const NetworkChange&) { throw 1; });
    const auto good = monitor.subscribe([&](const NetworkChange&) { ++good_calls; });
    std::this_thread::sleep_for(60ms);
    net.current = snap("10.9.9.9");
    for (int i = 0; i < 200 && good_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    monitor.unsubscribe(bad);
    monitor.unsubscribe(good);
    CHECK(good_calls.load() >= 1);
}

// Có thể đăng ký lại sau khi hết người đăng ký (luồng nền dừng rồi chạy lại).
void testResubscribeAfterStop() {
    FakeNet net{snap("192.168.1.5")};
    NetworkMonitor monitor(net.probe(), 5ms, 1);
    const auto first = monitor.subscribe([](const NetworkChange&) {});
    monitor.unsubscribe(first);
    std::atomic<int> calls{0};
    const auto second = monitor.subscribe([&](const NetworkChange&) { ++calls; });
    std::this_thread::sleep_for(60ms);
    net.current = snap("10.3.3.3");
    for (int i = 0; i < 200 && calls.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    monitor.unsubscribe(second);
    CHECK(calls.load() >= 1);
}

// Probe thật: không crash, địa chỉ đã sắp xếp, IP chính (nếu có) không phải loopback.
void testSystemProbeIsSane() {
    const NetworkSnapshot snapshot = NetworkMonitor::probe_system();
    CHECK(std::is_sorted(snapshot.addresses.begin(), snapshot.addresses.end()));
    CHECK(snapshot.primary_ip.rfind("127.", 0) != 0);
}

int main() {
    RUN(testBaselineAndNoChangeFiresNothing);
    RUN(testChangeNeedsConfirmationAndReportsBeforeAfter);
    RUN(testSingleFlapIsIgnored);
    RUN(testOfflineThenBackReportsOnce);
    RUN(testOfflineThenDifferentNetwork);
    RUN(testSecondaryAdapterOnlyIsNotPrimaryChange);
    RUN(testUnsubscribeWaitsForRunningCallback);
    RUN(testThrowingCallbackDoesNotBreakOthers);
    RUN(testResubscribeAfterStop);
    RUN(testSystemProbeIsSane);
    return TEST_REPORT();
}
