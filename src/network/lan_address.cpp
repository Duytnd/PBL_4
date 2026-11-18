#include "network/lan_address.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace p2p::network {
namespace {

bool is_valid_ipv4(const std::string& ip) {
    in_addr addr{};
    return !ip.empty() && inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool is_rfc1918(const std::string& ip) {
    if (!is_valid_ipv4(ip)) {
        return false;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

// Dùng được để quảng bá: IPv4 hợp lệ, không loopback, không link-local (169.254.x.x), không 0.0.0.0.
bool is_usable(const std::string& ip) {
    return is_valid_ipv4(ip) && !is_loopback_address(ip) && !starts_with(ip, "169.254.") &&
           ip != "0.0.0.0";
}

// Dải thường do card ảo (VirtualBox host-only, Docker) dùng, không phải LAN thật.
bool looks_virtual(const std::string& ip) {
    return starts_with(ip, "192.168.56.") || starts_with(ip, "192.168.99.") || starts_with(ip, "172.17.");
}

void ensure_winsock() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)initialized;
#endif
}

// IP nguồn mà hệ điều hành sẽ dùng để tới `target` (connect UDP không gửi gói nào).
// Trả "" nếu không có route (vd. LAN không có Internet).
std::string route_source_ip(const char* target) {
    ensure_winsock();
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        return {};
    }
#else
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return {};
    }
#endif
    std::string result;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    if (inet_pton(AF_INET, target, &remote.sin_addr) == 1 &&
        connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
#ifdef _WIN32
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        char buf[INET_ADDRSTRLEN];
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != nullptr) {
            result = buf;
        }
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return result;
}

std::vector<std::string> enumerate_local_ipv4() {
    std::vector<std::string> ips;
#ifdef _WIN32
    ensure_winsock();
    char host[256]{};
    if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &result) == 0) {
            for (const addrinfo* p = result; p != nullptr; p = p->ai_next) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                char buf[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
                    ips.emplace_back(buf);
                }
            }
            freeaddrinfo(result);
        }
    }
#else
    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) == 0) {
        for (const ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET ||
                (ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
                ips.emplace_back(buf);
            }
        }
        freeifaddrs(ifap);
    }
#endif
    return ips;
}

}  // namespace

bool is_loopback_address(const std::string& ip) {
    return starts_with(ip, "127.") || ip == "::1" || starts_with(ip, "::ffff:127.");
}

std::string pick_lan_ip(const std::vector<std::string>& candidates, const std::string& route_ip) {
    // 1) Route ra ngoài là IP private thật -> card đang dùng để đi mạng, tin nó nhất.
    if (is_usable(route_ip) && is_rfc1918(route_ip) && !looks_virtual(route_ip)) {
        return route_ip;
    }
    // 2) IP private đầu tiên của card mạng (bỏ card ảo trước).
    for (const auto& ip : candidates) {
        if (is_usable(ip) && is_rfc1918(ip) && !looks_virtual(ip)) {
            return ip;
        }
    }
    for (const auto& ip : candidates) {
        if (is_usable(ip) && is_rfc1918(ip)) {
            return ip;
        }
    }
    // 3) Không có IP private: dùng IP route ra ngoài (vd. LAN cấp IP public / Tailscale), rồi card nào dùng được.
    if (is_usable(route_ip)) {
        return route_ip;
    }
    for (const auto& ip : candidates) {
        if (is_usable(ip)) {
            return ip;
        }
    }
    return "127.0.0.1";
}

std::vector<std::string> local_ipv4_addresses(std::size_t max_count) {
    std::vector<std::string> real_private;
    std::vector<std::string> virtual_private;
    std::vector<std::string> others;
    for (const auto& ip : enumerate_local_ipv4()) {
        if (!is_usable(ip)) {
            continue;
        }
        auto& bucket = is_rfc1918(ip) ? (looks_virtual(ip) ? virtual_private : real_private) : others;
        if (std::find(bucket.begin(), bucket.end(), ip) == bucket.end()) {
            bucket.push_back(ip);
        }
    }
    std::vector<std::string> result;
    for (const auto* group : {&real_private, &virtual_private, &others}) {
        for (const auto& ip : *group) {
            if (result.size() < max_count && std::find(result.begin(), result.end(), ip) == result.end()) {
                result.push_back(ip);
            }
        }
    }
    return result;
}

bool is_local_network_address(const std::string& ip) {
    if (!is_valid_ipv4(ip)) {
        return false;
    }
    if (is_loopback_address(ip) || is_rfc1918(ip) || starts_with(ip, "169.254.")) {
        return true;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    return a == 100 && b >= 64 && b <= 127;
}

std::string default_gateway_ipv4() {
    std::string gateway;
#ifdef _WIN32
    ensure_winsock();
    in_addr target{};
    inet_pton(AF_INET, "8.8.8.8", &target);
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(target.S_un.S_addr, 0, &row) == NO_ERROR) {
        in_addr hop{};
        hop.S_un.S_addr = row.dwForwardNextHop;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &hop, buf, sizeof(buf)) != nullptr) {
            gateway = buf;
        }
    }
#elif defined(__APPLE__)
    if (FILE* pipe = popen("/sbin/route -n get default 2>/dev/null", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), pipe) != nullptr) {
            std::string text(line);
            const auto pos = text.find("gateway:");
            if (pos != std::string::npos) {
                std::istringstream parts(text.substr(pos + 8));
                parts >> gateway;
                break;
            }
        }
        pclose(pipe);
    }
#else
    std::ifstream routes("/proc/net/route");
    std::string row;
    std::getline(routes, row);  // dòng tiêu đề
    unsigned long best_metric = ~0UL;
    while (std::getline(routes, row)) {
        std::istringstream cols(row);
        std::string iface, destination, gw, flags, refcnt, use, metric;
        if (!(cols >> iface >> destination >> gw >> flags >> refcnt >> use >> metric)) {
            continue;
        }
        const unsigned long flag_bits = std::strtoul(flags.c_str(), nullptr, 16);
        if (destination != "00000000" || (flag_bits & 0x2) == 0) {  // route mặc định có cờ RTF_GATEWAY
            continue;
        }
        const unsigned long metric_value = std::strtoul(metric.c_str(), nullptr, 10);
        const unsigned long raw = std::strtoul(gw.c_str(), nullptr, 16);  // little-endian
        if (raw != 0 && metric_value <= best_metric) {
            best_metric = metric_value;
            gateway = std::to_string(raw & 0xFF) + "." + std::to_string((raw >> 8) & 0xFF) + "." +
                      std::to_string((raw >> 16) & 0xFF) + "." + std::to_string((raw >> 24) & 0xFF);
        }
    }
#endif
    // Chỉ tin gateway nằm trong mạng cục bộ (không phải loopback/0.0.0.0/địa chỉ lạ).
    if (!is_usable(gateway) || !is_local_network_address(gateway)) {
        return {};
    }
    return gateway;
}

std::string detect_lan_ip() {
    if (const char* forced = std::getenv("P2P_LAN_IP"); forced != nullptr && is_usable(forced)) {
        return forced;
    }
    return pick_lan_ip(enumerate_local_ipv4(), route_source_ip("8.8.8.8"));
}

std::string resolve_peer_ip(const std::string& remote_addr, const std::string& lan_ip) {
    if (is_loopback_address(remote_addr) && is_usable(lan_ip)) {
        return lan_ip;
    }
    return remote_addr;
}

}  // namespace p2p::network
