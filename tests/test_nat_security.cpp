#include "test_framework.h"

#include "network/lan_address.h"
#include "network/nat_traversal.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace pbl4::network;
using Clock = std::chrono::steady_clock;

namespace {

// Máy chủ TCP thử nghiệm trên loopback: `behavior` chạy trên 1 kết nối rồi giữ socket tới khi kết thúc test.
class FakeDevice {
public:
    template <typename Behavior>
    explicit FakeDevice(Behavior behavior)
        : acceptor_(ioc_, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)) {
        thread_ = std::thread([this, behavior]() {
            try {
                asio::ip::tcp::socket socket = acceptor_.accept();
                behavior(socket, stop_);
            } catch (...) {
            }
        });
    }
    ~FakeDevice() {
        stop_ = true;
        asio::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    asio::io_context ioc_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

void readRequest(asio::ip::tcp::socket& socket) {
    asio::streambuf buf;
    asio::read_until(socket, buf, "\r\n\r\n");
}

long long elapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

}  // namespace

void testSilentDeviceDoesNotHangTheCaller() {
    // Thiết bị nhận kết nối + request rồi im lặng mãi: trước đây vòng đọc không có hạn nên treo vĩnh viễn.
    FakeDevice device([](asio::ip::tcp::socket& socket, std::atomic<bool>& stop) {
        readRequest(socket);
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto start = Clock::now();
    const std::string response = NatTraversal::send_http_request(
        "127.0.0.1", device.port(), "GET", "/", "", "", std::chrono::milliseconds(600));
    CHECK(response.empty());
    CHECK(elapsedMs(start) < 2500);  // quanh 600ms, không phải vô hạn
}

void testDripFeedingDeviceIsStoppedByTotalDeadline() {
    // Nhỏ giọt 1 byte mỗi 100ms: hạn tính cho CẢ trao đổi nên không thể kéo dài mãi.
    FakeDevice device([](asio::ip::tcp::socket& socket, std::atomic<bool>& stop) {
        readRequest(socket);
        asio::write(socket, asio::buffer(std::string("HTTP/1.1 200 OK\r\n")));
        while (!stop) {
            asio::error_code ec;
            asio::write(socket, asio::buffer(std::string("x")), ec);
            if (ec) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const auto start = Clock::now();
    const std::string response = NatTraversal::send_http_request(
        "127.0.0.1", device.port(), "GET", "/", "", "", std::chrono::milliseconds(700));
    CHECK(response.empty());  // chưa có nổi header đầy đủ khi hết hạn
    CHECK(elapsedMs(start) < 2500);
}

void testFloodingDeviceIsBoundedBySizeCap() {
    // Thiết bị đẩy dữ liệu vô tận: phải bị cắt ở giới hạn kích thước, không phình bộ nhớ.
    FakeDevice device([](asio::ip::tcp::socket& socket, std::atomic<bool>& stop) {
        readRequest(socket);
        const std::string chunk(8192, 'A');
        while (!stop) {
            asio::error_code ec;
            asio::write(socket, asio::buffer(chunk), ec);
            if (ec) return;
        }
    });
    const auto start = Clock::now();
    const std::string response = NatTraversal::send_http_request(
        "127.0.0.1", device.port(), "GET", "/", "", "", std::chrono::milliseconds(5000));
    CHECK(response.empty());                       // quá lớn: bỏ hẳn
    CHECK(elapsedMs(start) < 3000);                // bị cắt sớm nhờ giới hạn, không chờ hết hạn 5 giây
}

void testKeepAliveDeviceReturnsAsSoonAsBodyIsComplete() {
    // Thiết bị gửi đủ Content-Length nhưng KHÔNG đóng kết nối (bỏ qua "Connection: close"): không được chờ hết hạn.
    FakeDevice device([](asio::ip::tcp::socket& socket, std::atomic<bool>& stop) {
        readRequest(socket);
        const std::string body = "<root>ok</root>";
        const std::string http = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\n\r\n" + body;
        asio::write(socket, asio::buffer(http));
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto start = Clock::now();
    const std::string response = NatTraversal::send_http_request(
        "127.0.0.1", device.port(), "GET", "/", "", "", std::chrono::milliseconds(5000));
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("<root>ok</root>") != std::string::npos);
    CHECK(elapsedMs(start) < 2000);
}

void testNormalCloseDelimitedResponseStillWorks() {
    FakeDevice device([](asio::ip::tcp::socket& socket, std::atomic<bool>&) {
        readRequest(socket);
        asio::write(socket, asio::buffer(std::string("HTTP/1.1 200 OK\r\n\r\nhello")));
        asio::error_code ec;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    });
    const std::string response = NatTraversal::send_http_request(
        "127.0.0.1", device.port(), "GET", "/", "", "", std::chrono::milliseconds(3000));
    CHECK(response.find("hello") != std::string::npos);
}

void testHostnamesAndBadHostsAreRejectedWithoutDns() {
    const auto start = Clock::now();
    CHECK(NatTraversal::send_http_request("localhost", 80, "GET", "/", "", "", std::chrono::milliseconds(500)).empty());
    CHECK(NatTraversal::send_http_request("evil.example.com", 80, "GET", "/", "", "", std::chrono::milliseconds(500)).empty());
    CHECK(NatTraversal::send_http_request("127.0.0.1", 0, "GET", "/", "", "", std::chrono::milliseconds(500)).empty());
    CHECK(NatTraversal::send_http_request("", 80, "GET", "/", "", "", std::chrono::milliseconds(500)).empty());
    CHECK(elapsedMs(start) < 1000);  // từ chối ngay, không tốn thời gian phân giải DNS
}

void testConnectionRefusedReturnsQuickly() {
    const auto start = Clock::now();
    // Cổng 1 trên loopback không ai nghe.
    CHECK(NatTraversal::send_http_request("127.0.0.1", 1, "GET", "/", "", "", std::chrono::milliseconds(3000)).empty());
    // Windows tự thử lại SYN tới cổng đóng ~2 giây; điều cần đảm bảo là không vượt hạn chót đã đặt.
    CHECK(elapsedMs(start) < 3600);
}

void testSsdpLocationMustMatchLocalSender() {
    std::string host, path;
    std::uint16_t port = 0;

    // Router thật: người gửi và LOCATION cùng 1 IP trong LAN.
    CHECK(NatTraversal::validate_ssdp_location("http://192.168.1.1:5000/rootDesc.xml", "192.168.1.1", host, port, path));
    CHECK_EQ(host, std::string("192.168.1.1"));
    CHECK_EQ(port, std::uint16_t(5000));
    CHECK_EQ(path, std::string("/rootDesc.xml"));
    CHECK(NatTraversal::validate_ssdp_location("http://10.0.0.1/desc.xml", "10.0.0.1", host, port, path));
    CHECK_EQ(port, std::uint16_t(80));
    CHECK(NatTraversal::validate_ssdp_location("http://100.64.0.1:49152/x", "100.64.0.1", host, port, path));  // CGNAT/Tailscale

    // SSRF: thiết bị tự xưng trỏ LOCATION sang máy khác.
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.50:8080/admin", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://127.0.0.1:8080/admin", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://169.254.169.254/latest/meta-data", "192.168.1.1", host, port, path));
    // Tên miền (DNS rebinding) và biến thể IP.
    CHECK(!NatTraversal::validate_ssdp_location("http://evil.example.com/desc.xml", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://localhost/desc.xml", "127.0.0.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1.evil.com/x", "192.168.1.1", host, port, path));
    // Người gửi là địa chỉ Internet công khai (không phải router trong nhà).
    CHECK(!NatTraversal::validate_ssdp_location("http://8.8.8.8/desc.xml", "8.8.8.8", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://203.0.113.5/desc.xml", "203.0.113.5", host, port, path));
    // Cổng/đường dẫn hỏng.
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1:99999/x", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1:0/x", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1:abc/x", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1/a b", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("https://192.168.1.1/x", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("", "192.168.1.1", host, port, path));
    CHECK(!NatTraversal::validate_ssdp_location("http://192.168.1.1/x", "not-an-ip", host, port, path));
}

void testLocalNetworkAddressClassification() {
    using p2p::network::is_local_network_address;
    CHECK(is_local_network_address("127.0.0.1"));
    CHECK(is_local_network_address("10.1.2.3"));
    CHECK(is_local_network_address("172.16.0.1"));
    CHECK(is_local_network_address("172.31.255.255"));
    CHECK(is_local_network_address("192.168.0.1"));
    CHECK(is_local_network_address("100.64.0.1"));
    CHECK(is_local_network_address("100.127.255.255"));
    CHECK(is_local_network_address("169.254.1.1"));
    CHECK(!is_local_network_address("172.15.0.1"));
    CHECK(!is_local_network_address("172.32.0.1"));
    CHECK(!is_local_network_address("100.63.0.1"));
    CHECK(!is_local_network_address("100.128.0.1"));
    CHECK(!is_local_network_address("8.8.8.8"));
    CHECK(!is_local_network_address("203.0.113.9"));
    CHECK(!is_local_network_address(""));
    CHECK(!is_local_network_address("evil.com"));
}

void testLocalInterfaceListIsSane() {
    const auto addresses = p2p::network::local_ipv4_addresses();
    CHECK(addresses.size() <= std::size_t(8));
    for (const auto& ip : addresses) {
        CHECK(ip.rfind("127.", 0) != 0);       // không loopback
        CHECK(ip.rfind("169.254.", 0) != 0);   // không link-local
        CHECK(ip != "0.0.0.0");
    }
    CHECK(p2p::network::local_ipv4_addresses(1).size() <= std::size_t(1));
}

// ---------------------------------------------------------------------------------------------------------
// NAT-PMP: gateway giả trên loopback + kẻ giả mạo trả lời sai trước khi gateway thật trả lời.
// ---------------------------------------------------------------------------------------------------------
namespace {

class FakeNatPmpGateway {
public:
    enum class Mode { Honest, ForgedFirst, WrongInternalEcho, WrongOpcode };

    explicit FakeNatPmpGateway(Mode mode)
        : mode_(mode),
          socket_(ioc_, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          attacker_(ioc_, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)) {
        socket_.non_blocking(true);
        thread_ = std::thread([this]() { serve(); });
    }
    ~FakeNatPmpGateway() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    std::uint16_t port() const { return socket_.local_endpoint().port(); }

private:
    static void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x >> 8));
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    }
    static void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int shift = 24; shift >= 0; shift -= 8) v.push_back(static_cast<std::uint8_t>((x >> shift) & 0xFF));
    }

    void serve() {
        std::array<std::uint8_t, 64> buf{};
        while (!stop_) {
            asio::ip::udp::endpoint from;
            asio::error_code ec;
            const std::size_t n = socket_.receive_from(asio::buffer(buf), from, 0, ec);
            if (ec || n < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const std::uint8_t opcode = buf[1];
            std::vector<std::uint8_t> reply;
            if (opcode == 0) {  // external IP
                reply = {0, 128, 0, 0};
                put32(reply, 1000);                            // epoch
                reply.insert(reply.end(), {203, 0, 113, 9});   // external IP
            } else if (opcode == 1 || opcode == 2) {
                const std::uint16_t internal_port = (static_cast<std::uint16_t>(buf[4]) << 8) | buf[5];
                const std::uint16_t requested_ext = (static_cast<std::uint16_t>(buf[6]) << 8) | buf[7];
                const std::uint8_t reply_opcode = mode_ == Mode::WrongOpcode
                    ? static_cast<std::uint8_t>(opcode == 1 ? 130 : 129)
                    : static_cast<std::uint8_t>(128 + opcode);
                reply = {0, reply_opcode, 0, 0};
                put32(reply, 1000);
                put16(reply, mode_ == Mode::WrongInternalEcho ? static_cast<std::uint16_t>(internal_port + 1) : internal_port);
                put16(reply, static_cast<std::uint16_t>(requested_ext + 1000));  // gateway cấp cổng ngoài khác cổng xin
                put32(reply, 3600);
            } else {
                continue;
            }
            if (mode_ == Mode::ForgedFirst) {
                // Kẻ giả mạo (cổng nguồn khác cổng của gateway) gửi phản hồi sai trước.
                std::vector<std::uint8_t> forged = reply;
                if (forged.size() >= 16) {
                    forged[10] = 0x27;  // cổng ngoài 9999
                    forged[11] = 0x0F;
                }
                if (forged.size() == 12) {
                    forged[8] = 6; forged[9] = 6; forged[10] = 6; forged[11] = 6;  // external IP 6.6.6.6
                }
                asio::error_code ignored;
                attacker_.send_to(asio::buffer(forged), from, 0, ignored);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            asio::error_code ignored;
            socket_.send_to(asio::buffer(reply), from, 0, ignored);
        }
    }

    Mode mode_;
    asio::io_context ioc_;
    asio::ip::udp::socket socket_;
    asio::ip::udp::socket attacker_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

void testNatPmpHonestGatewayDiscoveryAndMapping() {
    FakeNatPmpGateway gateway(FakeNatPmpGateway::Mode::Honest);
    NatTraversal nat;
    nat.configure_natpmp_gateway("127.0.0.1", gateway.port());
    CHECK(nat.discover_nat_pmp(std::chrono::milliseconds(1500)));
    CHECK_EQ(nat.external_ip(), std::string("203.0.113.9"));
    CHECK(nat.device_type() == NatDeviceType::NatPmp);

    const auto mapping = nat.map_port(6881, 6881, NatProtocol::TCP, 3600);
    CHECK(mapping.success);
    CHECK_EQ(mapping.external_port, std::uint16_t(7881));  // cổng gateway cấp, không phải cổng đã xin
    CHECK_EQ(mapping.lifetime, std::uint32_t(3600));
}

void testNatPmpIgnoresForgedRepliesFromOtherSenders() {
    FakeNatPmpGateway gateway(FakeNatPmpGateway::Mode::ForgedFirst);
    NatTraversal nat;
    nat.configure_natpmp_gateway("127.0.0.1", gateway.port());
    CHECK(nat.discover_nat_pmp(std::chrono::milliseconds(2000)));
    CHECK_EQ(nat.external_ip(), std::string("203.0.113.9"));  // không phải 6.6.6.6 của kẻ giả mạo

    const auto mapping = nat.map_port(6881, 6881, NatProtocol::TCP, 3600);
    CHECK(mapping.success);
    CHECK_EQ(mapping.external_port, std::uint16_t(7881));  // không phải 9999 của kẻ giả mạo
}

void testNatPmpRejectsMismatchedMapResponses() {
    {
        FakeNatPmpGateway gateway(FakeNatPmpGateway::Mode::WrongInternalEcho);
        NatTraversal nat;
        nat.configure_natpmp_gateway("127.0.0.1", gateway.port());
        CHECK(nat.discover_nat_pmp(std::chrono::milliseconds(1500)));
        CHECK(!nat.map_port(6881, 6881, NatProtocol::TCP, 3600).success);  // cổng nội bộ echo sai: phản hồi cho yêu cầu khác
    }
    {
        FakeNatPmpGateway gateway(FakeNatPmpGateway::Mode::WrongOpcode);
        NatTraversal nat;
        nat.configure_natpmp_gateway("127.0.0.1", gateway.port());
        CHECK(nat.discover_nat_pmp(std::chrono::milliseconds(1500)));
        CHECK(!nat.map_port(6881, 6881, NatProtocol::TCP, 3600).success);  // opcode UDP trả lời cho yêu cầu TCP
    }
}

void testNatPmpMapResponseValidatorUnit() {
    // Gói hợp lệ: version 0, opcode 130 (TCP), result 0, epoch, internal 6881, external 7000, lifetime 3600.
    const std::uint8_t good[16] = {0, 130, 0, 0, 0, 0, 0, 1, 0x1A, 0xE1, 0x1B, 0x58, 0, 0, 0x0E, 0x10};
    std::uint16_t ext = 0;
    std::uint32_t life = 0;
    CHECK(NatTraversal::parse_natpmp_map_response(good, 16, NatProtocol::TCP, 6881, ext, life));
    CHECK_EQ(ext, std::uint16_t(7000));
    CHECK_EQ(life, std::uint32_t(3600));
    CHECK(!NatTraversal::parse_natpmp_map_response(good, 16, NatProtocol::UDP, 6881, ext, life));  // sai giao thức
    CHECK(!NatTraversal::parse_natpmp_map_response(good, 16, NatProtocol::TCP, 6882, ext, life));  // sai cổng nội bộ
    CHECK(!NatTraversal::parse_natpmp_map_response(good, 15, NatProtocol::TCP, 6881, ext, life));  // quá ngắn

    std::uint8_t zero_port[16];
    std::memcpy(zero_port, good, 16);
    zero_port[10] = 0;
    zero_port[11] = 0;
    CHECK(!NatTraversal::parse_natpmp_map_response(zero_port, 16, NatProtocol::TCP, 6881, ext, life));
    CHECK(NatTraversal::parse_natpmp_map_response(zero_port, 16, NatProtocol::TCP, 6881, ext, life, /*allow_zero=*/true));  // xoá ánh xạ

    std::uint8_t refused[16];
    std::memcpy(refused, good, 16);
    refused[3] = 2;  // result code 2 = not authorized
    CHECK(!NatTraversal::parse_natpmp_map_response(refused, 16, NatProtocol::TCP, 6881, ext, life));
}

void testDefaultGatewayIsEmptyOrLocal() {
    // Gateway thật của hệ điều hành: rỗng nếu không xác định được, nếu có thì phải là địa chỉ mạng cục bộ.
    const std::string gateway = p2p::network::default_gateway_ipv4();
    CHECK(gateway.empty() || p2p::network::is_local_network_address(gateway));
    CHECK(gateway != "0.0.0.0");
    CHECK(gateway.rfind("127.", 0) != 0);
}

int main() {
    RUN(testSilentDeviceDoesNotHangTheCaller);
    RUN(testDripFeedingDeviceIsStoppedByTotalDeadline);
    RUN(testFloodingDeviceIsBoundedBySizeCap);
    RUN(testKeepAliveDeviceReturnsAsSoonAsBodyIsComplete);
    RUN(testNormalCloseDelimitedResponseStillWorks);
    RUN(testHostnamesAndBadHostsAreRejectedWithoutDns);
    RUN(testConnectionRefusedReturnsQuickly);
    RUN(testSsdpLocationMustMatchLocalSender);
    RUN(testLocalNetworkAddressClassification);
    RUN(testLocalInterfaceListIsSane);
    RUN(testNatPmpHonestGatewayDiscoveryAndMapping);
    RUN(testNatPmpIgnoresForgedRepliesFromOtherSenders);
    RUN(testNatPmpRejectsMismatchedMapResponses);
    RUN(testNatPmpMapResponseValidatorUnit);
    RUN(testDefaultGatewayIsEmptyOrLocal);
    return TEST_REPORT();
}
