#include "network/dht/kademlia_node.h"

#include "core/hash_utils.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <random>

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

}  // namespace

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

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
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

    socketHandle_ = static_cast<long long>(sock);
    running_ = true;
    receiveThread_ = std::thread(&KademliaNode::receiveLoop, this);
    return true;
}

void KademliaNode::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (socketHandle_ != -1) {
        closeSocketHandle(static_cast<SocketHandle>(socketHandle_));
        socketHandle_ = -1;
    }
}

void KademliaNode::receiveLoop() {
    std::vector<char> buffer(4096);

    while (running_) {
        sockaddr_in fromAddr{};
#ifdef _WIN32
        int fromLen = sizeof(fromAddr);
#else
        socklen_t fromLen = sizeof(fromAddr);
#endif
        const auto received = recvfrom(
            static_cast<SocketHandle>(socketHandle_), buffer.data(), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        if (received <= 0) {
            continue;  // timeout hoặc không có gì - vòng lại kiểm tra running_
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
        const std::uint16_t fromPort = ntohs(fromAddr.sin_port);
        const std::string payload(buffer.data(), static_cast<std::size_t>(received));

        try {
            handleIncoming(payload, ipStr, fromPort);
        } catch (const std::exception&) {
            // 1 node lạ gửi dữ liệu dị dạng không được làm sập cả node DHT
            // (cùng tinh thần "giới hạn phòng thủ" ở mục 5.3).
        }
    }
}

void KademliaNode::handleIncoming(const std::string& payload, const std::string& fromIp, std::uint16_t fromPort) {
    const KrpcMessage message = Krpc::decode(payload);

    routingTable_.insert(DhtContact{message.nodeId, fromIp, fromPort});

    if (message.type == KrpcType::Query) {
        handleQuery(message, fromIp, fromPort);
        return;
    }

    std::lock_guard<std::mutex> lock(pendingMutex_);
    const auto it = pendingCalls_.find(message.transactionId);
    if (it != pendingCalls_.end()) {
        it->second.response = message;
        it->second.done = true;
        pendingCv_.notify_all();
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
                const auto it = announcedPeers_.find(query.infoHash);
                if (it != announcedPeers_.end()) {
                    for (const auto& contact : it->second) {
                        peers.push_back({contact.ip, contact.port});
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
                auto& list = announcedPeers_[query.infoHash];
                const bool exists = std::any_of(list.begin(), list.end(), [&](const DhtContact& contact) {
                    return contact.ip == fromIp && contact.port == announcePort;
                });
                if (!exists) {
                    list.push_back(DhtContact{query.nodeId, fromIp, announcePort});
                }
            }
            sendRaw(fromIp, fromPort, Krpc::encodeAnnouncePeerResponse(query.transactionId, selfId_));
            break;
        }

        default:
            sendRaw(fromIp, fromPort, Krpc::encodeError(query.transactionId, 204, "Method unknown"));
    }
}

void KademliaNode::sendRaw(const std::string& host, std::uint16_t port, const std::string& payload) {
    if (socketHandle_ == -1) {
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr) {
        return;
    }

    sendto(static_cast<SocketHandle>(socketHandle_), payload.data(), static_cast<int>(payload.size()), 0,
           result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
}

std::string KademliaNode::makeTransactionId() {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string id(2, '\0');
    id[0] = static_cast<char>(dist(rng));
    id[1] = static_cast<char>(dist(rng));
    return id;
}

std::string KademliaNode::tokenFor(const std::string& ip, std::uint16_t port) const {
    return sha1Digest(ip + ":" + std::to_string(port) + ":" + secret_);
}

bool KademliaNode::sendAndWait(
    const std::string& host, std::uint16_t port, const std::string& transactionId,
    const std::string& payload, KrpcMessage& outResponse, std::chrono::milliseconds timeout) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCalls_[transactionId] = PendingCall{};
    }

    sendRaw(host, port, payload);

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
    if (!ping(host, port)) {
        return false;
    }
    findNode(selfId_);
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
            foundPeers.insert(foundPeers.end(), response.values.begin(), response.values.end());
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
    auto& list = announcedPeers_[infoHash];
    const bool exists = std::any_of(list.begin(), list.end(), [&](const DhtContact& contact) {
        return contact.id == selfId_;
    });
    if (!exists) {
        list.push_back(DhtContact{selfId_, "127.0.0.1", announcePort});
    }
}

}  // namespace p2p
