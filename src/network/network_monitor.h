#pragma once

// Theo dõi đổi mạng khi ứng dụng đang chạy (WiFi <-> LAN dây, đổi WiFi, mất mạng rồi có lại, bật/tắt VPN).
//
// Vấn đề: IP LAN, ánh xạ cổng UPnP/NAT-PMP, IP đã báo tracker/DHT, nhóm multicast LSD đều được chốt MỘT lần lúc khởi động.
// Đổi mạng giữa chừng thì các giá trị đó sai mà không ai biết. Lớp này dò định kỳ và báo cho ai đăng ký.
//
// Cách làm:
//  - Một luồng nền (chỉ chạy khi có người đăng ký) dò snapshot mạng mỗi `interval`.
//  - Chống nhiễu: trạng thái mới phải giống nhau `confirm_polls` lần dò liên tiếp mới được tính (lúc chuyển mạng
//    hệ điều hành hay báo trạng thái dở dang: mất IP, có IP link-local...).
//  - Mất mạng (không có IP dùng được) KHÔNG phát sự kiện; khi mạng quay lại mới phát một sự kiện duy nhất
//    (was_offline = true), kể cả khi IP không đổi (router khởi động lại có thể làm mất ánh xạ cổng).
//
// Callback chạy trên luồng của monitor, KHÔNG được gọi subscribe/unsubscribe bên trong callback (sẽ khoá chết) và nên
// chạy nhanh (chỉ đặt cờ). unsubscribe() chờ callback đang chạy xong trước khi trả về: sau đó callback không bao giờ
// được gọi nữa, nên đối tượng đăng ký có thể huỷ an toàn.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace p2p::network {

struct NetworkSnapshot {
    std::string primary_ip;              // IP LAN chính; rỗng = không có mạng dùng được
    std::vector<std::string> addresses;  // mọi IPv4 dùng được của máy, đã sắp xếp

    [[nodiscard]] bool online() const { return !primary_ip.empty(); }
    bool operator==(const NetworkSnapshot& other) const {
        return primary_ip == other.primary_ip && addresses == other.addresses;
    }
    bool operator!=(const NetworkSnapshot& other) const { return !(*this == other); }
};

struct NetworkChange {
    NetworkSnapshot before;      // snapshot online gần nhất trước sự kiện này
    NetworkSnapshot after;
    bool primary_changed = false;  // IP chính khác trước: kết nối cũ, ánh xạ cổng, IP đã báo đều không còn đúng
    bool was_offline = false;      // vừa có mạng lại sau một quãng mất mạng
};

class NetworkMonitor {
public:
    using Probe = std::function<NetworkSnapshot()>;
    using Callback = std::function<void(const NetworkChange&)>;

    // Dò thật từ hệ điều hành (detect_lan_ip + local_ipv4_addresses). Mất mạng/chỉ có loopback => primary_ip rỗng.
    static NetworkSnapshot probe_system();

    // Monitor dùng chung của tiến trình (dò thật, 3 giây/lần, xác nhận 2 lần liên tiếp => phát hiện sau ~6 giây).
    static NetworkMonitor& instance();

    // `probe` đưa vào được để kiểm thử. `confirm_polls` >= 1.
    explicit NetworkMonitor(Probe probe = &NetworkMonitor::probe_system,
                            std::chrono::milliseconds interval = std::chrono::seconds(3),
                            int confirm_polls = 2);
    ~NetworkMonitor();

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    // Đăng ký nhận sự kiện; luồng nền tự chạy khi có người đăng ký đầu tiên. Trả id để huỷ.
    std::uint64_t subscribe(Callback callback);
    // Huỷ đăng ký, chờ callback đang chạy (nếu có) xong. Luồng nền dừng khi hết người đăng ký. id lạ thì bỏ qua.
    void unsubscribe(std::uint64_t id);

    // Một lượt dò + xử lý đồng bộ (luồng nền gọi hàm này; kiểm thử gọi trực tiếp). Lần đầu chỉ đặt mốc.
    void poll_once();

    // CHỈ DÙNG TRONG KIỂM THỬ: thay probe/chu kỳ của monitor (kể cả instance() dùng chung). Mốc được đặt lại ở lượt dò
    // kế tiếp (lượt đó không phát sự kiện). Probe rỗng = quay lại probe_system.
    void configure_for_testing(Probe probe, std::chrono::milliseconds interval, int confirm_polls);

    // IP LAN chính gần nhất còn online (không rỗng khi mạng đang mất nhưng từng có). Tự đặt mốc nếu chưa dò lần nào.
    [[nodiscard]] std::string primary_ip();
    [[nodiscard]] NetworkSnapshot current();

private:
    void run();
    void notify(const NetworkChange& change);
    NetworkSnapshot run_probe();  // gọi probe_ (bản sao dưới khoá) rồi chạy ngoài khoá

    std::mutex config_mutex_;  // bảo vệ probe_ (đổi được trong kiểm thử)
    Probe probe_;
    std::atomic<long long> interval_ms_;
    std::atomic<int> confirm_polls_;

    std::mutex state_mutex_;  // bảo vệ các biến trạng thái dò bên dưới
    bool initialized_ = false;
    NetworkSnapshot stable_;       // trạng thái đã xác nhận
    NetworkSnapshot last_online_;  // snapshot online đã xác nhận gần nhất
    NetworkSnapshot candidate_;    // trạng thái đang chờ xác nhận
    int candidate_count_ = 0;

    std::mutex lifecycle_mutex_;  // tuần tự hoá subscribe/unsubscribe (và khởi/dừng luồng)
    std::mutex subs_mutex_;
    std::map<std::uint64_t, std::shared_ptr<Callback>> subscribers_;
    std::uint64_t next_id_ = 1;
    std::mutex callback_mutex_;  // giữ khi đang gọi callback; unsubscribe khoá/mở nó để chờ callback đang chạy

    std::thread thread_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool stop_requested_ = false;
};

}  // namespace p2p::network
