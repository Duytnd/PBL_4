#pragma once

#include <string>
#include <vector>

namespace p2p::network {

// Chọn IPv4 LAN của máy này để quảng bá cho peer/tracker.
//
// Ưu tiên: biến môi trường P2P_LAN_IP (ép tay) > IP nguồn của route ra Internet (nếu private)
// > IP private đầu tiên của các card mạng (card ảo VirtualBox/Docker xếp sau) > mọi IPv4 dùng
// được > "127.0.0.1". Khác cách cũ (chỉ hỏi route tới 8.8.8.8), mạng LAN không có Internet
// vẫn ra đúng IP thay vì rơi về loopback.
std::string detect_lan_ip();

// Phần chọn thuần túy (không đụng mạng) để test được.
// candidates: IPv4 của các card mạng; route_ip: IP nguồn của route ra ngoài ("" nếu không có).
std::string pick_lan_ip(const std::vector<std::string>& candidates, const std::string& route_ip);

// true cho 127.0.0.0/8, ::1 và ::ffff:127.x.
bool is_loopback_address(const std::string& ip);

// IP cần ghi cho 1 peer kết nối tới tracker: nếu peer nối từ loopback (cùng máy với tracker) thì
// máy khác trong LAN không với tới "127.0.0.1" được, nên thay bằng IP LAN của tracker.
// lan_ip rỗng hoặc cũng là loopback thì giữ nguyên remote_addr.
std::string resolve_peer_ip(const std::string& remote_addr, const std::string& lan_ip);

// IPv4 của các card mạng đang hoạt động (bỏ loopback và link-local 169.254.x.x), tối đa `max_count` địa chỉ.
// Thứ tự: IP private của card thật trước, card ảo (VirtualBox/Docker) sau, rồi mọi IP còn lại.
// Dùng để gửi dò tìm (SSDP/UPnP) trên TỪNG card: máy nhiều card (VPN, Tailscale, VM) chọn nhầm card mặc định
// thì gói multicast không bao giờ tới được router.
std::vector<std::string> local_ipv4_addresses(std::size_t max_count = 8);

// true nếu là địa chỉ trong mạng cục bộ: loopback, RFC1918, 100.64/10 (CGNAT/Tailscale) hoặc link-local 169.254/16.
// Dùng để từ chối thiết bị "router" tự xưng từ địa chỉ Internet công khai.
bool is_local_network_address(const std::string& ip);

// Gateway mặc định THẬT của hệ điều hành (hop kế tiếp của route ra Internet), "" nếu không xác định được hoặc
// không phải địa chỉ mạng cục bộ. Windows: GetBestRoute; Linux: /proc/net/route; macOS: `route -n get default`.
// Trước đây NAT-PMP chỉ đoán 3 địa chỉ cố định (192.168.1.1, 192.168.0.1, 10.0.0.1).
std::string default_gateway_ipv4();

}  // namespace p2p::network
