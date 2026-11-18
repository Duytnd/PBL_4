#include "network/tracker_client.h"

#include "httplib.h"

#include <algorithm>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace p2p {
namespace {

constexpr int kTrackerMaxRetries = 3;

bool isTransientTrackerHttpStatus(int status) {
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

void sleepBeforeRetry(int attempt) {
    const auto delayMs = std::chrono::milliseconds(250 * attempt);
    std::this_thread::sleep_for(delayMs);
}

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
    bool tls = false;
};

// Parser tối thiểu cho announce URL dạng "http(s)://host[:port][/path]".
// UDP tracker (BEP 15) đi qua announceUdp().
ParsedUrl parseHttpUrl(const std::string& url) {
    const bool tls = url.rfind("https://", 0) == 0;
    const std::string prefix = tls ? "https://" : "http://";
    if (!tls && url.rfind(prefix, 0) != 0) {
        throw std::invalid_argument("TrackerClient: only http:// and https:// announce URLs are supported, got: " + url);
    }

    const std::string rest = url.substr(prefix.size());
    const std::size_t pathStart = rest.find('/');
    const std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);

    ParsedUrl parsed;
    parsed.tls = tls;
    parsed.port = tls ? 443 : 80;
    parsed.path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    const std::size_t colon = hostPort.find(':');
    if (colon == std::string::npos) {
        parsed.host = hostPort;
    } else {
        parsed.host = hostPort.substr(0, colon);
        parsed.port = std::stoi(hostPort.substr(colon + 1));
    }

    if (parsed.host.empty()) {
        throw std::invalid_argument("TrackerClient: announce URL is missing a host: " + url);
    }

    return parsed;
}


// --- UDP tracker (BEP 15) -------------------------------------------------

constexpr std::uint64_t kUdpProtocolId = 0x41727101980ULL;
constexpr std::uint32_t kUdpActionConnect = 0;
constexpr std::uint32_t kUdpActionAnnounce = 1;
constexpr std::uint32_t kUdpActionError = 3;
constexpr int kUdpTimeoutMs = 5000;
constexpr int kUdpMaxAttempts = 3;
constexpr std::size_t kUdpMaxPacket = 4096;

void putU16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void putU32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void putU64(std::string& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

std::uint32_t getU32(const std::string& in, std::size_t pos) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(in[pos + i]);
    }
    return v;
}

std::uint64_t getU64(const std::string& in, std::size_t pos) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(in[pos + i]);
    }
    return v;
}

std::uint32_t udpEventCode(const std::string& event) {
    if (event == "completed") return 1;
    if (event == "started") return 2;
    if (event == "stopped") return 3;
    return 0;
}

// Parser cho "udp://host:port[/path]" (port bắt buộc theo BEP 15).
ParsedUrl parseUdpUrl(const std::string& url) {
    const std::string prefix = "udp://";
    if (url.rfind(prefix, 0) != 0) {
        throw std::invalid_argument("TrackerClient: not a udp:// announce URL: " + url);
    }
    const std::string rest = url.substr(prefix.size());
    const std::size_t pathStart = rest.find('/');
    const std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);
    const std::size_t colon = hostPort.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= hostPort.size()) {
        throw std::invalid_argument("TrackerClient: udp announce URL needs host:port: " + url);
    }
    ParsedUrl parsed;
    parsed.host = hostPort.substr(0, colon);
    try {
        parsed.port = std::stoi(hostPort.substr(colon + 1));
    } catch (const std::exception&) {
        throw std::invalid_argument("TrackerClient: invalid udp tracker port: " + url);
    }
    if (parsed.port <= 0 || parsed.port > 65535) {
        throw std::invalid_argument("TrackerClient: udp tracker port out of range: " + url);
    }
    return parsed;
}


bool isIpv4Literal(const std::string& host, int octets[4]) {
    if (host.empty() || std::count(host.begin(), host.end(), '.') != 3) {
        return false;
    }
    std::size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t end = i == 3 ? host.size() : host.find('.', pos);
        if (end == std::string::npos || end == pos || end - pos > 3) return false;
        const std::string part = host.substr(pos, end - pos);
        if (part.find_first_not_of("0123456789") != std::string::npos) return false;
        const int value = std::stoi(part);
        if (value > 255) return false;
        octets[i] = value;
        pos = end + 1;
    }
    return true;
}

bool isSensitivePort(int port) {
    static const int kBlocked[] = {20, 21, 22, 23, 25, 110, 111, 135, 137, 138, 139, 143, 445, 465, 587, 993, 995,
                                   1433, 1521, 2049, 2375, 3306, 3389, 5432, 5900, 6379, 9200, 11211, 27017};
    return std::find(std::begin(kBlocked), std::end(kBlocked), port) != std::end(kBlocked);
}

#ifdef _WIN32
using UdpSocket = SOCKET;
constexpr UdpSocket kInvalidUdpSocket = INVALID_SOCKET;
void closeUdpSocket(UdpSocket s) { closesocket(s); }
#else
using UdpSocket = int;
constexpr UdpSocket kInvalidUdpSocket = -1;
void closeUdpSocket(UdpSocket s) { ::close(s); }
#endif

// Socket UDP đã connect() tới tracker, tự đóng khi hết phạm vi.
class UdpChannel {
public:
    UdpChannel(const std::string& host, int port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        addrinfo hints{};
        hints.ai_family = AF_INET;  // peer BEP 15 mặc định là IPv4 (6 byte/peer)
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
            throw std::runtime_error("TrackerClient: cannot resolve udp tracker host " + host);
        }
        socket_ = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (socket_ == kInvalidUdpSocket) {
            freeaddrinfo(result);
            throw std::runtime_error("TrackerClient: cannot create UDP socket");
        }
        const int rc = ::connect(socket_, result->ai_addr, static_cast<int>(result->ai_addrlen));
        freeaddrinfo(result);
        if (rc != 0) {
            closeUdpSocket(socket_);
            socket_ = kInvalidUdpSocket;
            throw std::runtime_error("TrackerClient: cannot connect UDP socket to " + host);
        }
#ifdef _WIN32
        const DWORD timeout = kUdpTimeoutMs;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval tv{};
        tv.tv_sec = kUdpTimeoutMs / 1000;
        tv.tv_usec = (kUdpTimeoutMs % 1000) * 1000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    ~UdpChannel() {
        if (socket_ != kInvalidUdpSocket) {
            closeUdpSocket(socket_);
        }
    }
    UdpChannel(const UdpChannel&) = delete;
    UdpChannel& operator=(const UdpChannel&) = delete;

    bool send(const std::string& packet) {
        return ::send(socket_, packet.data(), static_cast<int>(packet.size()), 0) ==
               static_cast<int>(packet.size());
    }

    // Trả chuỗi rỗng nếu timeout / lỗi.
    std::string receive() {
        std::string buffer(kUdpMaxPacket, '\0');
        const int n = ::recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (n <= 0) {
            return {};
        }
        buffer.resize(static_cast<std::size_t>(n));
        return buffer;
    }

private:
    UdpSocket socket_ = kInvalidUdpSocket;
};

std::uint32_t randomU32() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_int_distribution<std::uint32_t>()(rng);
}

}  // namespace

std::string TrackerClient::buildAnnounceRequest(const TrackerRequest& request) {
    return TrackerProtocol::encodeRequestQuery(request);
}

std::string TrackerClient::makePeerId(const std::string& prefix) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist('a', 'z');

    std::string generated;
    generated.reserve(prefix.size() + 12);
    generated.append(prefix);
    for (int i = 0; i < 12; ++i) {
        generated.push_back(static_cast<char>(dist(rng)));
    }
    return generated;
}

TrackerResponse TrackerClient::announce(const std::string& responseBody, const TrackerRequest& request) {
    (void)request;
    return parseAnnounceResponse(responseBody);
}

TrackerResponse TrackerClient::parseAnnounceResponse(const std::string& responseBody) {
    return TrackerProtocol::decodeResponse(responseBody);
}

TrackerUrlCheck TrackerClient::checkTrackerUrl(const std::string& url) {
    auto reject = [](const std::string& why) { return TrackerUrlCheck{false, why}; };

    std::string host;
    int port = 0;
    std::string path;
    try {
        if (url.rfind("udp://", 0) == 0) {
            const ParsedUrl parsed = parseUdpUrl(url);
            host = parsed.host;
            port = parsed.port;
        } else if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            const ParsedUrl parsed = parseHttpUrl(url);
            host = parsed.host;
            port = parsed.port;
            path = parsed.path;
        } else {
            return reject("only http://, https:// and udp:// trackers are supported");
        }
    } catch (const std::exception& e) {
        return reject(e.what());
    }

    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (host.empty() || port <= 0 || port > 65535) {
        return reject("missing host or invalid port");
    }
    if (host == "metadata.google.internal" || host == "metadata" || host == "instance-data") {
        return reject("cloud metadata host");
    }
    int o[4] = {0, 0, 0, 0};
    if (isIpv4Literal(host, o)) {
        if (o[0] == 0) return reject("unspecified address");
        if (o[0] == 169 && o[1] == 254) return reject("link-local address (cloud metadata)");
        if (o[0] >= 224) return reject("multicast or broadcast address");
    }
    if (isSensitivePort(port)) {
        return reject("port belongs to a sensitive service");
    }
    if (url.rfind("udp://", 0) != 0) {
        std::string segment = path.substr(0, path.find('?'));
        const std::size_t slash = segment.find_last_of('/');
        segment = slash == std::string::npos ? segment : segment.substr(slash + 1);
        std::transform(segment.begin(), segment.end(), segment.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // Tracker công khai hiếm gặp dùng tên khác (vd /a, /tracker): bật rõ ràng bằng P2P_TRACKER_ANY_PATH=1.
        const char* anyPath = std::getenv("P2P_TRACKER_ANY_PATH");
        const bool allowAnyPath = anyPath != nullptr && std::string(anyPath) == "1";
        if (!allowAnyPath && segment.rfind("announce", 0) != 0) {
            return reject("HTTP tracker path must end with an 'announce' segment");
        }
    }
    return TrackerUrlCheck{true, {}};
}

TrackerResponse TrackerClient::announceHttp(const std::string& announceUrl, const TrackerRequest& request) {
    if (const auto check = checkTrackerUrl(announceUrl); !check.allowed) {
        throw std::invalid_argument("TrackerClient: tracker URL rejected (" + check.reason + "): " + announceUrl);
    }
    const ParsedUrl url = parseHttpUrl(announceUrl);
    const std::string query = TrackerProtocol::encodeRequestQuery(request);

    for (int attempt = 1; attempt <= kTrackerMaxRetries; ++attempt) {
        httplib::Result result{nullptr, httplib::Error::Unknown};
        if (url.tls) {
            // HTTPS: luôn kiểm tra chứng chỉ và tên máy chủ (không có đường tắt để tắt). Chứng chỉ gốc lấy từ kho của
            // hệ điều hành; tracker riêng dùng CA tự cấp thì trỏ P2P_TRACKER_CA_FILE tới file PEM của CA đó.
            httplib::SSLClient client(url.host, url.port);
            client.set_connection_timeout(5);
            client.set_read_timeout(5);
            client.enable_server_certificate_verification(true);
            client.enable_server_hostname_verification(true);
            if (const char* ca = std::getenv("P2P_TRACKER_CA_FILE"); ca != nullptr && *ca != '\0') {
                client.set_ca_cert_path(ca);
            }
            result = client.Get(url.path + "?" + query);
        } else {
            httplib::Client client(url.host, url.port);
            client.set_connection_timeout(5);
            client.set_read_timeout(5);
            result = client.Get(url.path + "?" + query);
        }
        if (!result) {
            if (attempt < kTrackerMaxRetries) {
                sleepBeforeRetry(attempt);
                continue;
            }
            throw std::runtime_error(
                "TrackerClient: failed to reach tracker at " + announceUrl +
                " (" + httplib::to_string(result.error()) + ")");
        }
        if (result->status == 200) {
            return parseAnnounceResponse(result->body);
        }
        if (isTransientTrackerHttpStatus(result->status) && attempt < kTrackerMaxRetries) {
            sleepBeforeRetry(attempt);
            continue;
        }
        throw std::runtime_error(
            "TrackerClient: tracker responded with HTTP " + std::to_string(result->status));
    }

    throw std::runtime_error("TrackerClient: tracker announce failed after retries");
}

std::string TrackerClient::buildUdpConnectRequest(std::uint32_t transactionId) {
    std::string packet;
    packet.reserve(16);
    putU64(packet, kUdpProtocolId);
    putU32(packet, kUdpActionConnect);
    putU32(packet, transactionId);
    return packet;
}

bool TrackerClient::parseUdpConnectResponse(const std::string& packet, std::uint32_t transactionId,
                                            std::uint64_t& connectionId, std::string& errorMessage) {
    if (packet.size() < 8) {
        return false;
    }
    const std::uint32_t action = getU32(packet, 0);
    if (getU32(packet, 4) != transactionId) {
        return false;
    }
    if (action == kUdpActionError) {
        errorMessage = packet.substr(8);
        return false;
    }
    if (action != kUdpActionConnect || packet.size() < 16) {
        return false;
    }
    connectionId = getU64(packet, 8);
    return true;
}

std::string TrackerClient::buildUdpAnnounceRequest(std::uint64_t connectionId, std::uint32_t transactionId,
                                                   const TrackerRequest& request, std::uint32_t key) {
    if (request.infoHash.size() != 20 || request.peerId.size() != 20) {
        throw std::invalid_argument("TrackerClient: info_hash and peer_id must be 20 bytes for UDP announce");
    }
    std::string packet;
    packet.reserve(98);
    putU64(packet, connectionId);
    putU32(packet, kUdpActionAnnounce);
    putU32(packet, transactionId);
    packet.append(request.infoHash);
    packet.append(request.peerId);
    putU64(packet, request.downloaded);
    putU64(packet, request.left);
    putU64(packet, request.uploaded);
    putU32(packet, udpEventCode(request.event));
    putU32(packet, 0);  // IP: 0 = tracker tự lấy địa chỉ nguồn
    putU32(packet, key);
    putU32(packet, request.numwant > 0 ? static_cast<std::uint32_t>(request.numwant) : 0xFFFFFFFFu);
    putU16(packet, request.port);
    return packet;
}

TrackerResponse TrackerClient::parseUdpAnnounceResponse(const std::string& packet, std::uint32_t transactionId) {
    TrackerResponse response;
    if (packet.size() < 8 || getU32(packet, 4) != transactionId) {
        response.failureReason = "UDP tracker: malformed or mismatched announce response";
        return response;
    }
    const std::uint32_t action = getU32(packet, 0);
    if (action == kUdpActionError) {
        response.failureReason = packet.substr(8);
        return response;
    }
    if (action != kUdpActionAnnounce || packet.size() < 20) {
        response.failureReason = "UDP tracker: unexpected announce response";
        return response;
    }
    response.interval = static_cast<int>(getU32(packet, 8));
    for (std::size_t pos = 20; pos + 6 <= packet.size(); pos += 6) {
        TrackerPeer peer;
        peer.ip = std::to_string(static_cast<std::uint8_t>(packet[pos])) + "." +
                  std::to_string(static_cast<std::uint8_t>(packet[pos + 1])) + "." +
                  std::to_string(static_cast<std::uint8_t>(packet[pos + 2])) + "." +
                  std::to_string(static_cast<std::uint8_t>(packet[pos + 3]));
        peer.port = static_cast<std::uint16_t>(
            (static_cast<std::uint8_t>(packet[pos + 4]) << 8) | static_cast<std::uint8_t>(packet[pos + 5]));
        if (peer.port != 0) {
            response.peers.push_back(std::move(peer));
        }
    }
    response.success = true;
    return response;
}

TrackerResponse TrackerClient::announceUdp(const std::string& announceUrl, const TrackerRequest& request) {
    if (const auto check = checkTrackerUrl(announceUrl); !check.allowed) {
        throw std::invalid_argument("TrackerClient: tracker URL rejected (" + check.reason + "): " + announceUrl);
    }
    const ParsedUrl url = parseUdpUrl(announceUrl);
    UdpChannel channel(url.host, url.port);
    const std::uint32_t key = randomU32();

    std::string lastError = "no response";
    for (int attempt = 1; attempt <= kUdpMaxAttempts; ++attempt) {
        // 1) connect: lấy connection_id (tracker nhớ ~1 phút nên mỗi lần announce xin lại).
        const std::uint32_t connectTx = randomU32();
        if (!channel.send(buildUdpConnectRequest(connectTx))) {
            lastError = "send failed";
            continue;
        }
        const std::string connectReply = channel.receive();
        if (connectReply.empty()) {
            lastError = "connect timed out";
            continue;
        }
        std::uint64_t connectionId = 0;
        std::string errorMessage;
        if (!parseUdpConnectResponse(connectReply, connectTx, connectionId, errorMessage)) {
            if (!errorMessage.empty()) {
                throw std::runtime_error("TrackerClient: UDP tracker error: " + errorMessage);
            }
            lastError = "invalid connect response";
            continue;
        }

        // 2) announce
        const std::uint32_t announceTx = randomU32();
        if (!channel.send(buildUdpAnnounceRequest(connectionId, announceTx, request, key))) {
            lastError = "send failed";
            continue;
        }
        const std::string announceReply = channel.receive();
        if (announceReply.empty()) {
            lastError = "announce timed out";
            continue;
        }
        TrackerResponse response = parseUdpAnnounceResponse(announceReply, announceTx);
        if (response.success) {
            return response;
        }
        // Tracker từ chối rõ ràng (action=error, đúng transactionId): trả lỗi, không retry.
        if (announceReply.size() >= 8 && getU32(announceReply, 0) == kUdpActionError &&
            getU32(announceReply, 4) == announceTx) {
            return response;
        }
        lastError = response.failureReason;
    }
    throw std::runtime_error("TrackerClient: UDP tracker " + announceUrl + " failed (" + lastError + ")");
}

TrackerResponse TrackerClient::announceTracker(const std::string& announceUrl, const TrackerRequest& request) {
    if (announceUrl.rfind("udp://", 0) == 0) {
        return announceUdp(announceUrl, request);
    }
    return announceHttp(announceUrl, request);
}

std::pair<std::string, std::uint16_t> TrackerClient::udpRendezvousEndpoint(
    const std::string& announceUrl) {
    const ParsedUrl url = parseHttpUrl(announceUrl);
    if (url.port <= 0 || url.port > 65535) {
        throw std::invalid_argument("TrackerClient: announce port is outside the UDP range");
    }
    return {url.host, static_cast<std::uint16_t>(url.port)};
}

}  // namespace p2p
