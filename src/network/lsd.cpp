#include "network/lsd.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>

namespace p2p {

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }

struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
void ensureWinsock() { static WinsockInit w; (void)w; }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) { ::close(s); }
void ensureWinsock() {}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// LocalServiceDiscovery
// ---------------------------------------------------------------------------

LocalServiceDiscovery::LocalServiceDiscovery(std::uint16_t listenPort)
    : listenPort_(listenPort), cookie_(makeCookie()) {}

LocalServiceDiscovery::~LocalServiceDiscovery() {
    stop();
}

std::string LocalServiceDiscovery::makeCookie() {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string c(4, '\0');
    for (auto& b : c) {
        b = static_cast<char>(dist(rng));
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(8);
    for (unsigned char b : c) {
        out += hex[(b >> 4) & 0xF];
        out += hex[b & 0xF];
    }
    return out;
}

std::string LocalServiceDiscovery::toHex(const std::string& raw) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char b : raw) {
        out += hex[(b >> 4) & 0xF];
        out += hex[b & 0xF];
    }
    return out;
}

bool LocalServiceDiscovery::parseMessage(
        const std::string& msg, const std::string& fromIp, LsdPeer& out) {
    // Thông điệp phải bắt đầu bằng "BT-SEARCH"
    if (msg.rfind("BT-SEARCH", 0) != 0) {
        return false;
    }

    std::string port;
    std::string infoHash;

    std::istringstream ss(msg);
    std::string line;
    while (std::getline(ss, line)) {
        // Xóa \r nếu có
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim leading spaces trong value
        const auto firstNonSpace = value.find_first_not_of(' ');
        if (firstNonSpace != std::string::npos) {
            value = value.substr(firstNonSpace);
        } else {
            value.clear();
        }

        // Convert key to lowercase for case-insensitive comparison
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (key == "port") {
            port = value;
        } else if (key == "infohash") {
            infoHash = value;
        }
    }

    if (port.empty() || infoHash.empty()) {
        return false;
    }

    // Validate: port phải là số, infoHash phải là 40 ký tự hex
    if (infoHash.size() != 40) {
        return false;
    }
    for (char c : infoHash) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    try {
        const int p = std::stoi(port);
        if (p <= 0 || p > 65535) {
            return false;
        }
        out.port = static_cast<std::uint16_t>(p);
    } catch (...) {
        return false;
    }

    out.ip = fromIp;
    out.infoHashHex = infoHash;
    // Normalize hex to lowercase
    std::transform(out.infoHashHex.begin(), out.infoHashHex.end(),
                   out.infoHashHex.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return true;
}

bool LocalServiceDiscovery::start(LsdCallback callback) {
    if (running_) {
        return true;
    }

    ensureWinsock();
    callback_ = std::move(callback);

    // Tạo socket UDP multicast
    const SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        return false;
    }

    // SO_REUSEADDR (+ SO_REUSEPORT trên hệ thống hỗ trợ) để nhiều process cùng lắng nghe
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif

    // Bind vào INADDR_ANY:kMulticastPort
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(kMulticastPort);

    if (bind(sock, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
        closeSocket(sock);
        return false;
    }

    // Đăng ký multicast group
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kMulticastGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    // Timeout nhận để running_ có thể kiểm tra được (ngắn để stop() không phải chờ lâu)
#ifdef _WIN32
    DWORD timeoutMs = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval tv{0, 100000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    socketHandle_.store(static_cast<long long>(sock));
    running_ = true;
    receiveThread_ = std::thread(&LocalServiceDiscovery::receiveLoop, this);
    return true;
}

void LocalServiceDiscovery::stop() {
    // Lần gọi thứ hai (kể cả đồng thời) chờ ở đây tới khi lần đầu dọn xong, không trả về sớm.
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    if (!running_.exchange(false)) {
        return;
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    const long long handle = socketHandle_.exchange(-1);
    if (handle != -1) {
        closeSocket(static_cast<SocketHandle>(handle));
    }
}

bool LocalServiceDiscovery::rejoinMulticast() {
    const long long handle = socketHandle_.load();
    if (!running_ || handle == -1) {
        return false;
    }
    const SocketHandle sock = static_cast<SocketHandle>(handle);
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kMulticastGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;
    // Bỏ tư cách thành viên cũ (lỗi nếu card đó đã biến mất: bỏ qua) rồi tham gia lại trên card hiện tại.
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    return setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
}

void LocalServiceDiscovery::receiveLoop() {
    std::vector<char> buffer(2048);
    auto errorBackoff = std::chrono::milliseconds(100);  // lỗi thật liên tục: ngủ 100 ms, lùi dần tới 1 giây

    while (running_) {
        sockaddr_in fromAddr{};
#ifdef _WIN32
        int fromLen = sizeof(fromAddr);
#else
        socklen_t fromLen = sizeof(fromAddr);
#endif
        const long long handle = socketHandle_.load();
        if (handle == -1) {
            break;
        }
        const auto received = recvfrom(
            static_cast<SocketHandle>(handle),
            buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        if (received <= 0) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            // Timeout là chủ đích; ECONNRESET trên UDP chỉ là ICMP "cổng đóng" của một lần gửi trước.
            const bool benign = err == WSAETIMEDOUT || err == WSAECONNRESET || err == WSAEINTR || err == WSAEMSGSIZE;
#else
            const int err = errno;
            const bool benign = err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNREFUSED;
#endif
            if (!benign && running_) {
                // Lỗi cứng liên tục (vd. WSAENETDOWN): ngủ thay vì đốt CPU.
                std::this_thread::sleep_for(errorBackoff);
                errorBackoff = std::min(errorBackoff * 2, std::chrono::milliseconds(1000));
            }
            continue;
        }
        errorBackoff = std::chrono::milliseconds(100);

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));

        const std::string payload(buffer.data(), static_cast<std::size_t>(received));

        // Lọc bỏ echo của chính mình (kiểm tra cookie)
        if (payload.find("cookie: " + cookie_) != std::string::npos) {
            continue;
        }

        LsdPeer peer;
        try {
            if (parseMessage(payload, ipStr, peer) && callback_) {
                callback_(peer);
            }
        } catch (...) {
            // Không để một thông điệp lỗi làm sập cả loop
        }
    }
}

bool LocalServiceDiscovery::announce(const std::string& infoHashRaw) {
    if (infoHashRaw.size() != 20) {
        return false;
    }

    ensureWinsock();

    const SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        return false;
    }

    // Cho phép multicast loopback (để test trên cùng máy)
    unsigned char loopback = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loopback), sizeof(loopback));

    // TTL = 1: chỉ trong subnet local (đủ cho LAN 3 laptop)
    unsigned char ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    const std::string hexHash = toHex(infoHashRaw);

    // Tạo thông điệp BT-SEARCH theo chuẩn BEP 14
    std::ostringstream msg;
    msg << "BT-SEARCH * HTTP/1.1\r\n"
        << "Host: " << kMulticastGroup << ":" << kMulticastPort << "\r\n"
        << "Port: " << listenPort_ << "\r\n"
        << "Infohash: " << hexHash << "\r\n"
        << "cookie: " << cookie_ << "\r\n"
        << "\r\n"
        << "\r\n";

    const std::string payload = msg.str();

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(kMulticastGroup);
    dest.sin_port = htons(kMulticastPort);

    const auto sent = sendto(sock, payload.data(), static_cast<int>(payload.size()), 0,
                             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

    closeSocket(sock);
    return sent > 0;
}

}  // namespace p2p

