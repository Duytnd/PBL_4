#pragma once

// Local Service Discovery — BEP 14
// https://www.bittorrent.org/beps/bep_0014.html
//
// Cho phép các client BitTorrent tự tìm nhau trong cùng mạng LAN mà không cần
// tracker hay DHT, bằng cách gửi multicast HTTP-like announce định kỳ.
//
// Cơ chế:
//   - Gửi UDP multicast tới 239.192.152.143:6771 (IPv4) một thông điệp dạng:
//       BT-SEARCH * HTTP/1.1\r\n
//       Host: 239.192.152.143:6771\r\n
//       Port: <port>\r\n
//       Infohash: <hex-info-hash>\r\n
//       cookie: <random>\r\n
//       \r\n
//       \r\n
//   - Bất kỳ client nào đang lắng nghe cùng multicast group, đang tải cùng
//     info_hash, sẽ phát hiện peer mới và kết nối trực tiếp.
//
// Giới hạn (phù hợp quy mô 3 laptop LAN của dự án, mục 2 "ngoài phạm vi"):
//   - Chỉ IPv4.
//   - Không xử lý TTL hay multi-interface; bind vào INADDR_ANY.
//   - announce() gửi 1 lần; vòng lặp tự động không nằm trong lớp này —
//     caller (torrent_session hoặc test) tự gọi định kỳ.

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace p2p {

// Thông tin một peer phát hiện qua LSD.
struct LsdPeer {
    std::string ip;
    std::uint16_t port = 0;
    std::string infoHashHex;  // 40 ký tự hex
};

// Callback khi phát hiện peer mới qua LSD.
using LsdCallback = std::function<void(const LsdPeer&)>;

class LocalServiceDiscovery {
public:
    static constexpr const char* kMulticastGroup = "239.192.152.143";
    static constexpr std::uint16_t kMulticastPort = 6771;

    // port: cổng TCP peer wire của client này (để thông báo cho peer khác kết nối vào).
    explicit LocalServiceDiscovery(std::uint16_t listenPort);
    ~LocalServiceDiscovery();

    LocalServiceDiscovery(const LocalServiceDiscovery&) = delete;
    LocalServiceDiscovery& operator=(const LocalServiceDiscovery&) = delete;

    // Bắt đầu lắng nghe multicast. Gọi callback mỗi khi có peer mới được phát hiện.
    // Trả false nếu không bind được socket.
    bool start(LsdCallback callback);
    void stop();

    // Gửi 1 announce multicast cho info_hash (dạng 20 byte raw, không phải hex).
    // Không cần start() trước — announce() hoạt động độc lập qua socket riêng.
    bool announce(const std::string& infoHashRaw);

    [[nodiscard]] bool isRunning() const { return running_; }

private:
    std::uint16_t listenPort_;
    std::string cookie_;           // chuỗi ngẫu nhiên để lọc echo của chính mình
    std::atomic<bool> running_{false};
    std::thread receiveThread_;
    long long socketHandle_ = -1;  // tránh include winsock/socket trong header

    LsdCallback callback_;

    void receiveLoop();
    static std::string makeCookie();

    // Parse thông điệp BT-SEARCH và trích xuất LsdPeer.
    // Trả false nếu không phải định dạng hợp lệ hoặc thiếu trường bắt buộc.
    static bool parseMessage(const std::string& msg, const std::string& fromIp, LsdPeer& out);

    // Chuyển 20 byte raw -> 40 ký tự hex thường.
    static std::string toHex(const std::string& raw);
};

}  // namespace p2p

