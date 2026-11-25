#include "network/dht/kademlia_node.h"

#include "core/hash_utils.h"
#include "network/lan_address.h"
#include "network/network_monitor.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <string>

namespace p2p {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocketHandle(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocketHandle(SocketHandle s) { close(s); }
#endif

// Khởi tạo Winsock đúng 1 lần cho toàn tiến trình (no-op trên Linux/Mac).
struct WinsockInitializer {
    WinsockInitializer() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~WinsockInitializer() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

void ensureWinsockInitialized() {
    static WinsockInitializer instance;
    (void)instance;
}

std::vector<DhtContact> sortedByDistance(std::vector<DhtContact> contacts, const NodeId& target, std::size_t limit) {
    std::sort(contacts.begin(), contacts.end(), [&](const DhtContact& a, const DhtContact& b) {
        return a.id.distanceTo(target) < b.id.distanceTo(target);
    });
    if (contacts.size() > limit) {
        contacts.resize(limit);
    }
    return contacts;
}

bool is_valid_ipv4(const std::string& ip) {
    if (ip.empty()) {
        return false;
    }
    struct in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool is_loopback_or_invalid_ip(const std::string& ip) {
    if (!is_valid_ipv4(ip)) {
        return true;
    }
    return ip == "127.0.0.1" || ip == "0.0.0.0" || ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0;
}

std::string detectLocalLanIp() {
    return network::detect_lan_ip();
}

}  // namespace

bool KademliaNode::is_private_ipv4(const std::string& ip) {
    if (!is_valid_ipv4(ip) || is_loopback_or_invalid_ip(ip)) {
        return false;
    }

    std::uint32_t octet1 = 0;
    std::uint32_t octet2 = 0;
    std::uint32_t octet3 = 0;
    std::uint32_t octet4 = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &octet1, &octet2, &octet3, &octet4) != 4) {
        return false;
    }

    // RFC 1918 + 100.64.0.0/10 (CGNAT / Tailscale): coi là mạng nội bộ, không phải Internet công khai.
    return (octet1 == 10) ||
           (octet1 == 172 && octet2 >= 16 && octet2 <= 31) ||
           (octet1 == 192 && octet2 == 168) ||
           (octet1 == 100 && octet2 >= 64 && octet2 <= 127);
}

bool KademliaNode::is_public_ipv4(const std::string& ip) {
    return is_valid_ipv4(ip) && !is_loopback_or_invalid_ip(ip) && !is_private_ipv4(ip);
}

bool KademliaNode::should_connect_peer(
    const std::string& local_ip,
    const std::string& candidate_ip,
    bool has_private_peer_alternative,
    bool allow_public_peers) {
    if (candidate_ip.empty()) {
        return false;
    }
    if (candidate_ip == "127.0.0.1" || candidate_ip == "0.0.0.0") {
        return true;
    }
    if (is_loopback_or_invalid_ip(candidate_ip)) {
        return false;
    }

    if (is_private_ipv4(candidate_ip)) {
        return true;
    }

    // Mạng được cấp IP public (mạng trường/lab) hoặc người dùng chủ động muốn nối peer Internet:
    // bộ lọc LAN bị tắt, mọi peer hợp lệ đều được nối.
    if (allow_public_peers) {
        return true;
    }

    // Nếu máy đang ở mạng private/LAN: ưu tiên private peer, public chỉ được
    // nếu không có private alternative nào. Nếu máy không ở private LAN, tuyệt
    // đối không add public peer từ DHT/PEX; chỉ kết nối đúng peer đã biết rõ.
    if (local_ip.empty() || !is_private_ipv4(local_ip) || local_ip == "127.0.0.1") {
        return false;
    }

    return !has_private_peer_alternative;
}

KademliaNode::KademliaNode(NodeId selfId, std::uint16_t port)
    : selfId_(selfId), port_(port), secret_(NodeId::random().toString()), routingTable_(selfId) {}

KademliaNode::~KademliaNode() {
    stop();
}

bool KademliaNode::start() {
    ensureWinsockInitialized();

    const SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        return false;
    }

#ifndef _WIN32
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[DHT] bind failed on port " << port_ << ": " << strerror(errno) << std::endl;
        closeSocketHandle(sock);
        return false;
    }

    // Timeout cho recvfrom để vòng lặp nhận kiểm tra running_ định kỳ, không
    // treo mãi khi stop() được gọi.
#ifdef _WIN32
    DWORD timeoutMs = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval tv{0, 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    socketHandle_.store(static_cast<long long>(sock));
    const bool autoDetectIp = localIp().empty();
    if (autoDetectIp) {
        setLocalIp(detectLocalLanIp());
    }
    running_ = true;
    receiveThread_ = std::thread(&KademliaNode::receiveLoop, this);
    // IP tự dò thì phải theo kịp khi đổi mạng (WiFi <-> dây, đổi WiFi...). IP đặt tay (setLocalIp trước start) giữ nguyên.
    if (autoDetectIp) {
        networkSubscription_ = network::NetworkMonitor::instance().subscribe(
            [this](const network::NetworkChange& change) {
                if (!change.after.primary_ip.empty()) {
                    setLocalIp(change.after.primary_ip);
                    std::cout << "[DHT] Network changed: local IP is now " << change.after.primary_ip << std::endl;
                }
            });
    }
    return true;
}

void KademliaNode::stop() {
    // Lần gọi thứ hai (kể cả đồng thời) chờ ở đây tới khi lần đầu dọn xong, không trả về sớm.
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    if (!running_.exchange(false)) {
        return;
    }
    if (networkSubscription_ != 0) {
        network::NetworkMonitor::instance().unsubscribe(networkSubscription_);  // chờ callback đang chạy xong
        networkSubscription_ = 0;
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    // Đợi sendRaw đang gửi dở (luồng khác) xong rồi mới đóng; sau đó handle = -1 nên sendRaw không gửi nữa.
    std::lock_guard<std::mutex> socketLock(socketMutex_);
    const long long handle = socketHandle_.exchange(-1);
    if (handle != -1) {
        closeSocketHandle(static_cast<SocketHandle>(handle));
    }
}

void KademliaNode::receiveLoop() {
    std::vector<char> buffer(4096);
    // Log chẩn đoán: chỉ in lần đầu thấy mỗi nguồn gửi, và giới hạn số gói lỗi in ra,
    // để node DHT công khai không làm tràn console.
    constexpr std::size_t kMaxLoggedSenders = 256;
    std::set<std::string> loggedSenders;
    std::size_t malformedCount = 0;
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
            static_cast<SocketHandle>(handle), buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        if (received <= 0) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            // Timeout là chủ đích (500 ms, để kiểm tra running_). WSAECONNRESET trên UDP chỉ là ICMP "cổng đóng"
            // của một lần gửi trước, không phải lỗi socket.
            const bool benign = err == WSAETIMEDOUT || err == WSAECONNRESET || err == WSAEINTR || err == WSAEMSGSIZE;
#else
            const int err = errno;
            const bool benign = err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNREFUSED;
#endif
            if (!benign && running_) {
                // Lỗi cứng liên tục (vd. WSAENETDOWN): ngủ thay vì đốt CPU trong vòng lặp.
                std::this_thread::sleep_for(errorBackoff);
                errorBackoff = std::min(errorBackoff * 2, std::chrono::milliseconds(1000));
            }
            continue;
        }
        errorBackoff = std::chrono::milliseconds(100);

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
        const std::uint16_t fromPort = ntohs(fromAddr.sin_port);
        const std::string payload(buffer.data(), static_cast<std::size_t>(received));

        const std::string sender = std::string(ipStr) + ":" + std::to_string(fromPort);
        if (loggedSenders.size() < kMaxLoggedSenders && loggedSenders.insert(sender).second) {
            std::cout << "[DHT] first packet from " << sender << " (" << received << " bytes)" << std::endl;
        }

        try {
            handleIncoming(payload, ipStr, fromPort);
        } catch (const std::exception& e) {
            // 1 node lạ gửi dữ liệu dị dạng không được làm sập cả node DHT
            // (cùng tinh thần "giới hạn phòng thủ" ở mục 5.3).
            ++malformedCount;
            if (malformedCount <= 20 || malformedCount % 100 == 0) {
                std::cerr << "[DHT] dropped malformed packet from " << sender << ": " << e.what()
                          << " (total dropped: " << malformedCount << ")" << std::endl;
            }
        }
    }
}

void KademliaNode::handleIncoming(const std::string& payload, const std::string& fromIp, std::uint16_t fromPort) {
    const KrpcMessage message = Krpc::decode(payload);

    if (message.type == KrpcType::Query) {
        routingTable_.insert(DhtContact{message.nodeId, fromIp, fromPort});
        handleQuery(message, fromIp, fromPort);
        return;
    }

    std::lock_guard<std::mutex> lock(pendingMutex_);
    const auto it = pendingCalls_.find(message.transactionId);
    // Chỉ nhận phản hồi từ đúng địa chỉ đã được hỏi: transaction id chỉ 2 byte nên người ngoài đoán/giả mạo được.
    if (it != pendingCalls_.end() && !it->second.done && it->second.expectedPort != 0 &&
        it->second.expectedIp == fromIp && it->second.expectedPort == fromPort) {
        it->second.response = message;
        it->second.done = true;
        pendingCv_.notify_all();
        // Phản hồi giả/không được hỏi (sai địa chỉ) không được chen vào routing table.
        routingTable_.insert(DhtContact{message.nodeId, fromIp, fromPort});
    }
}

void KademliaNode::handleQuery(const KrpcMessage& query, const std::string& fromIp, std::uint16_t fromPort) {
    switch (query.method) {
        case KrpcMethod::Ping: {
            sendRaw(fromIp, fromPort, Krpc::encodePingResponse(query.transactionId, selfId_));
            break;
        }

        case KrpcMethod::FindNode: {
            const auto closest = routingTable_.closestTo(query.target, RoutingTable::kBucketSize);
            std::vector<KrpcNodeInfo> nodes;
            nodes.reserve(closest.size());
            for (const auto& contact : closest) {
                nodes.push_back({contact.id, contact.ip, contact.port});
            }
            sendRaw(fromIp, fromPort, Krpc::encodeFindNodeResponse(query.transactionId, selfId_, nodes));
            break;
        }

        case KrpcMethod::GetPeers: {
            const std::string token = tokenFor(fromIp, fromPort);
            std::vector<KrpcPeerInfo> peers;
            {
                std::lock_guard<std::mutex> lock(announcedMutex_);
                const auto now = std::chrono::steady_clock::now();
                const auto it = announcedPeers_.find(query.infoHash);
                if (it != announcedPeers_.end()) {
                    for (const auto& entry : it->second) {
                        const DhtContact& contact = entry.contact;
                        if (contact.id != selfId_ && now - entry.at > kAnnounceTtl) {
                            continue;  // hết hạn, sẽ được dọn ở lần announce kế tiếp
                        }
                        std::string peerIp = contact.ip;
                        if ((peerIp == "127.0.0.1" || peerIp.empty() || contact.id == selfId_) &&
                            fromIp != "127.0.0.1" && fromIp != "localhost") {
                            const std::string selfIp = localIp();
                            peerIp = !selfIp.empty() ? selfIp : detectLocalLanIp();
                        }
                        peers.push_back({peerIp, contact.port});
                    }
                }
            }

            if (!peers.empty()) {
                sendRaw(fromIp, fromPort,
                        Krpc::encodeGetPeersResponseWithPeers(query.transactionId, selfId_, token, peers));
            } else if (query.infoHash.size() == NodeId::kByteLength) {
                const auto closest = routingTable_.closestTo(NodeId::fromString(query.infoHash), RoutingTable::kBucketSize);
                std::vector<KrpcNodeInfo> nodes;
                nodes.reserve(closest.size());
                for (const auto& contact : closest) {
                    nodes.push_back({contact.id, contact.ip, contact.port});
                }
                sendRaw(fromIp, fromPort,
                        Krpc::encodeGetPeersResponseWithNodes(query.transactionId, selfId_, token, nodes));
            } else {
                sendRaw(fromIp, fromPort, Krpc::encodeError(query.transactionId, 203, "info_hash must be 20 bytes"));
            }
            break;
        }

        case KrpcMethod::AnnouncePeer: {
            const std::string expectedToken = tokenFor(fromIp, fromPort);
            if (query.token != expectedToken) {
                sendRaw(fromIp, fromPort, Krpc::encodeError(query.transactionId, 203, "Bad token"));
                break;
            }

            const std::uint16_t announcePort = query.impliedPort ? fromPort : query.port;
            {
                std::lock_guard<std::mutex> lock(announcedMutex_);
                const auto now = std::chrono::steady_clock::now();
                pruneAnnouncedLocked(now);
                auto& list = announcedPeers_[query.infoHash];
                const auto existing = std::find_if(list.begin(), list.end(), [&](const AnnouncedPeer& entry) {
                    return entry.contact.ip == fromIp && entry.contact.port == announcePort;
                });
                if (existing != list.end()) {
                    existing->at = now;  // announce lại: gia hạn
                } else if (announcedTotal_ < kMaxAnnouncedTotal) {
                    if (list.size() >= kMaxAnnouncedPerHash) {
                        // Đầy theo info_hash: bỏ peer cũ nhất (trừ bản ghi của chính node này) để nhường chỗ.
                        const auto oldest = std::find_if(list.begin(), list.end(), [&](const AnnouncedPeer& entry) {
                            return entry.contact.id != selfId_;
                        });
                        if (oldest != list.end()) {
                            list.erase(oldest);
                            --announcedTotal_;
                        }
                    }
                    if (list.size() < kMaxAnnouncedPerHash) {
                        list.push_back(AnnouncedPeer{DhtContact{query.nodeId, fromIp, announcePort}, now});
                        ++announcedTotal_;
                    }
                }
                if (list.empty()) {
                    announcedPeers_.erase(query.infoHash);
                }
            }
            sendRaw(fromIp, fromPort, Krpc::encodeAnnouncePeerResponse(query.transactionId, selfId_));
            break;
        }

        default:
            sendRaw(fromIp, fromPort, Krpc::encodeError(query.transactionId, 204, "Method unknown"));
    }
}

void KademliaNode::pruneAnnouncedLocked(std::chrono::steady_clock::time_point now) {
    // Dọn tối đa mỗi 10 giây (không quét toàn bộ ở mỗi lần chèn). Bản ghi của chính node này không hết hạn.
    if (lastAnnouncedPrune_ != std::chrono::steady_clock::time_point{} &&
        now - lastAnnouncedPrune_ < std::chrono::seconds(10)) {
        return;
    }
    lastAnnouncedPrune_ = now;
    for (auto it = announcedPeers_.begin(); it != announcedPeers_.end();) {
        auto& list = it->second;
        const auto before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const AnnouncedPeer& entry) {
                       return entry.contact.id != selfId_ && now - entry.at > kAnnounceTtl;
                   }),
                   list.end());
        announcedTotal_ -= before - list.size();
        it = list.empty() ? announcedPeers_.erase(it) : std::next(it);
    }
}

void KademliaNode::sendRaw(const std::string& host, std::uint16_t port, const std::string& payload) {
    if (socketHandle_.load() == -1) {
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr) {
        std::cerr << "[DHT] cannot resolve " << host << ":" << port << " (need an IPv4 address or hostname)"
                  << std::endl;
        return;
    }

    long long sent = -1;
    {
        std::lock_guard<std::mutex> socketLock(socketMutex_);
        const long long handle = socketHandle_.load();
        if (handle == -1) {
            freeaddrinfo(result);
            return;
        }
        sent = sendto(static_cast<SocketHandle>(handle), payload.data(),
                      static_cast<int>(payload.size()), 0, result->ai_addr,
                      static_cast<int>(result->ai_addrlen));
    }
    if (sent < 0) {
#ifdef _WIN32
        const int err = WSAGetLastError();
#else
        const int err = errno;
#endif
        std::cerr << "[DHT] sendto " << host << ":" << port << " failed, error " << err << std::endl;
    }
    freeaddrinfo(result);
}

std::string KademliaNode::makeTransactionId() {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    std::string id(2, '\0');
    std::lock_guard<std::mutex> lock(pendingMutex_);
    // Tránh id đang được truy vấn khác dùng; đặt chỗ ngay để hai luồng không cùng lấy một id.
    // Không gian id chỉ 65536: nếu gần cạn thì chấp nhận id trùng (sendAndWait sẽ ghi đè) thay vì lặp mãi.
    for (int attempt = 0; attempt < 64; ++attempt) {
        id[0] = static_cast<char>(dist(rng));
        id[1] = static_cast<char>(dist(rng));
        if (pendingCalls_.find(id) == pendingCalls_.end()) {
            break;
        }
    }
    pendingCalls_[id] = PendingCall{};
    return id;
}

std::string KademliaNode::tokenFor(const std::string& ip, std::uint16_t port) const {
    return sha1Digest(ip + ":" + std::to_string(port) + ":" + secret_);
}

bool KademliaNode::sendAndWait(
    const std::string& host, std::uint16_t port, const std::string& transactionId,
    const std::string& payload, KrpcMessage& outResponse, std::chrono::milliseconds timeout) {
    // Phân giải đích thành IPv4 một lần: phản hồi phải đến từ đúng địa chỉ này (xem handleIncoming).
    std::string targetIp = host;
    if (!is_valid_ipv4(targetIp)) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingCalls_.erase(transactionId);
            std::cerr << "[DHT] cannot resolve " << host << ":" << port << " (need an IPv4 address or hostname)"
                      << std::endl;
            return false;
        }
        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr, ipStr, sizeof(ipStr));
        freeaddrinfo(result);
        targetIp = ipStr;
    }
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        PendingCall call;
        call.expectedIp = targetIp;
        call.expectedPort = port;
        pendingCalls_[transactionId] = std::move(call);
    }

    sendRaw(targetIp, port, payload);

    std::unique_lock<std::mutex> lock(pendingMutex_);
    const bool arrived = pendingCv_.wait_for(lock, timeout, [&]() {
        return pendingCalls_[transactionId].done;
    });

    bool success = false;
    if (arrived) {
        outResponse = pendingCalls_[transactionId].response;
        success = (outResponse.type == KrpcType::Response);
    }
    pendingCalls_.erase(transactionId);
    return success;
}

bool KademliaNode::ping(const std::string& host, std::uint16_t port) {
    const std::string transactionId = makeTransactionId();
    const std::string payload = Krpc::encodePing(transactionId, selfId_);

    KrpcMessage response;
    if (!sendAndWait(host, port, transactionId, payload, response)) {
        return false;
    }
    routingTable_.insert(DhtContact{response.nodeId, host, port});
    return true;
}

bool KademliaNode::bootstrap(const std::string& host, std::uint16_t port) {
    std::cout << "[DHT] bootstrap: ping " << host << ":" << port << " (local UDP port " << port_ << ")"
              << std::endl;
    const auto started = std::chrono::steady_clock::now();
    if (!ping(host, port)) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started).count();
        std::cerr << "[DHT] bootstrap: no reply from " << host << ":" << port << " after " << elapsedMs
                  << " ms (wrong port, host down, NAT/firewall, or not a DHT node); will not retry"
                  << std::endl;
        return false;
    }
    const std::size_t before = routingTable_.size();
    findNode(selfId_);
    std::cout << "[DHT] bootstrap: " << host << ":" << port << " replied; routing table " << before << " -> "
              << routingTable_.size() << " nodes" << std::endl;
    return true;
}

std::vector<DhtContact> KademliaNode::findNode(const NodeId& target) {
    std::vector<DhtContact> shortlist = routingTable_.closestTo(target, RoutingTable::kBucketSize);
    std::vector<NodeId> queried;

    for (int round = 0; round < 8 && !shortlist.empty(); ++round) {
        bool progressed = false;

        // Lặp trên 1 bản sao vì shortlist có thể được nối thêm trong lúc lặp.
        const std::vector<DhtContact> toQuery = shortlist;
        for (const auto& contact : toQuery) {
            if (std::find(queried.begin(), queried.end(), contact.id) != queried.end()) {
                continue;
            }
            queried.push_back(contact.id);

            const std::string transactionId = makeTransactionId();
            const std::string payload = Krpc::encodeFindNode(transactionId, selfId_, target);
            KrpcMessage response;
            if (!sendAndWait(contact.ip, contact.port, transactionId, payload, response)) {
                continue;
            }
            progressed = true;

            routingTable_.insert(DhtContact{response.nodeId, contact.ip, contact.port});
            for (const auto& node : response.nodes) {
                routingTable_.insert(DhtContact{node.id, node.ip, node.port});
                shortlist.push_back(DhtContact{node.id, node.ip, node.port});
            }
        }

        if (!progressed) {
            break;
        }
        shortlist = sortedByDistance(shortlist, target, RoutingTable::kBucketSize);
    }

    return shortlist;
}

std::vector<KrpcPeerInfo> KademliaNode::getPeers(const std::string& infoHash) {
    const NodeId target = NodeId::fromString(infoHash);
    std::vector<DhtContact> shortlist = routingTable_.closestTo(target, RoutingTable::kBucketSize);
    std::vector<NodeId> queried;
    std::vector<KrpcPeerInfo> foundPeers;
    std::vector<std::pair<DhtContact, std::string>> tokens;

    for (int round = 0; round < 8 && !shortlist.empty(); ++round) {
        bool progressed = false;
        const std::vector<DhtContact> toQuery = shortlist;

        for (const auto& contact : toQuery) {
            if (std::find(queried.begin(), queried.end(), contact.id) != queried.end()) {
                continue;
            }
            queried.push_back(contact.id);

            const std::string transactionId = makeTransactionId();
            const std::string payload = Krpc::encodeGetPeers(transactionId, selfId_, infoHash);
            KrpcMessage response;
            if (!sendAndWait(contact.ip, contact.port, transactionId, payload, response)) {
                continue;
            }
            progressed = true;

            routingTable_.insert(DhtContact{response.nodeId, contact.ip, contact.port});
            if (!response.token.empty()) {
                tokens.emplace_back(DhtContact{response.nodeId, contact.ip, contact.port}, response.token);
            }
            for (auto p : response.values) {
                if ((p.ip == "127.0.0.1" || p.ip.empty()) && contact.ip != "127.0.0.1") {
                    p.ip = contact.ip;
                }
                if (!KademliaNode::should_connect_peer(localIp(), p.ip, false)) {
                    continue;
                }
                foundPeers.push_back(p);
            }
            for (const auto& node : response.nodes) {
                routingTable_.insert(DhtContact{node.id, node.ip, node.port});
                shortlist.push_back(DhtContact{node.id, node.ip, node.port});
            }
        }

        if (!progressed) {
            break;
        }
        shortlist = sortedByDistance(shortlist, target, RoutingTable::kBucketSize);
    }

    if (is_private_ipv4(localIp())) {
        const bool has_private_peer = std::any_of(
            foundPeers.begin(), foundPeers.end(), [](const KrpcPeerInfo& peer) {
                return KademliaNode::is_private_ipv4(peer.ip);
            });
        if (has_private_peer) {
            foundPeers.erase(
                std::remove_if(
                    foundPeers.begin(), foundPeers.end(),
                    [](const KrpcPeerInfo& peer) {
                        return !KademliaNode::is_private_ipv4(peer.ip);
                    }),
                foundPeers.end());
        }
    }

    {
        std::lock_guard<std::mutex> lock(lastLookupMutex_);
        lastLookupTokens_ = tokens;
    }

    return foundPeers;
}

void KademliaNode::announcePeer(const std::string& infoHash, std::uint16_t announcePort) {
    std::vector<std::pair<DhtContact, std::string>> tokens;
    {
        std::lock_guard<std::mutex> lock(lastLookupMutex_);
        tokens = lastLookupTokens_;
    }

    for (const auto& entry : tokens) {
        const DhtContact& contact = entry.first;
        const std::string& token = entry.second;

        const std::string transactionId = makeTransactionId();
        const std::string payload = Krpc::encodeAnnouncePeer(transactionId, selfId_, infoHash, announcePort, token);
        KrpcMessage response;
        sendAndWait(contact.ip, contact.port, transactionId, payload, response);
    }

    // Tự ghi nhận chính mình đang seed info_hash này để trả lời get_peers từ node khác.
    std::lock_guard<std::mutex> lock(announcedMutex_);
    const auto now = std::chrono::steady_clock::now();
    pruneAnnouncedLocked(now);
    auto& list = announcedPeers_[infoHash];
    const std::string configuredIp = localIp();
    const std::string myIp = !configuredIp.empty() ? configuredIp : detectLocalLanIp();
    auto self = std::find_if(list.begin(), list.end(), [&](const AnnouncedPeer& entry) {
        return entry.contact.id == selfId_;
    });
    if (self == list.end()) {
        // Bản ghi của chính mình luôn được nhận (nhường chỗ nếu info_hash đã đầy bởi peer ngoài).
        if (list.size() >= kMaxAnnouncedPerHash && !list.empty()) {
            list.erase(list.begin());
            --announcedTotal_;
        }
        list.push_back(AnnouncedPeer{DhtContact{selfId_, myIp, announcePort}, now});
        ++announcedTotal_;
    } else {
        // Torrent bị xóa rồi thêm lại sẽ nghe ở cổng TCP mới: luôn làm mới cổng, nếu không DHT mãi trả cổng cũ đã đóng.
        self->contact.ip = myIp;
        self->contact.port = announcePort;
        self->at = now;
    }
}

}  // namespace p2p
