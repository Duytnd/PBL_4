#pragma once

#include "network/dht/krpc.h"
#include "network/dht/node_id.h"
#include "network/dht/routing_table.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace p2p {

// 1 node DHT thật, chạy trên UDP (BEP 5). Dùng RoutingTable + Krpc đã viết
// sẵn ở trên; đây là phần nối chúng với mạng thật + vòng lặp xử lý message.
//
// Đơn giản hoá so với Kademlia "sách giáo khoa" (phù hợp quy mô 15-30 peer ảo
// của mục 5.6, không nhằm production-grade - mục 2 "ngoài phạm vi"):
//   - RPC đồng bộ: gửi query rồi chờ response trong 1 khoảng timeout (không
//     làm hàng loạt request song song không đồng bộ).
//   - Token cho get_peers/announce_peer tính bằng SHA-1(ip:port:secret),
//     không xoay secret theo thời gian như triển khai đầy đủ.
class KademliaNode {
public:
    KademliaNode(NodeId selfId, std::uint16_t port);
    ~KademliaNode();

    KademliaNode(const KademliaNode&) = delete;
    KademliaNode& operator=(const KademliaNode&) = delete;

    // Bind UDP socket + chạy thread nhận message. Trả false nếu bind lỗi.
    bool start();
    void stop();

    // Gửi ping, true nếu nhận pong trong timeout.
    bool ping(const std::string& host, std::uint16_t port);

    // Ping + find_node(selfId) tới 1 node đã biết để nhập routing table vào mạng.
    bool bootstrap(const std::string& host, std::uint16_t port);

    // Lặp find_node dần về target (iterative lookup, BEP 5), tự thêm node mới
    // tìm được vào routing table. Trả về các contact gần target nhất tìm được.
    std::vector<DhtContact> findNode(const NodeId& target);

    // get_peers: hỏi dần các node gần info_hash nhất, trả peer thật nếu có.
    std::vector<KrpcPeerInfo> getPeers(const std::string& infoHash);

    // Công bố: node này đang seed info_hash tại announcePort. Gọi getPeers()
    // trước để có token hợp lệ từ các node gần nhất, sau đó announce_peer tới
    // từng node đó.
    void announcePeer(const std::string& infoHash, std::uint16_t announcePort);

    [[nodiscard]] RoutingTable& routingTable() { return routingTable_; }
    [[nodiscard]] const NodeId& selfId() const { return selfId_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    struct PendingCall {
        bool done = false;
        KrpcMessage response;
    };

    NodeId selfId_;
    std::uint16_t port_;
    std::string secret_;

    std::atomic<bool> running_{false};
    std::thread receiveThread_;
    long long socketHandle_ = -1;  // tránh include winsock trong header

    RoutingTable routingTable_;

    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;
    std::map<std::string, PendingCall> pendingCalls_;

    std::mutex announcedMutex_;
    std::map<std::string, std::vector<DhtContact>> announcedPeers_;  // info_hash -> peer đã announce tới node này

    // Node gần nhất tìm được lần getPeers() gần nhất, để announcePeer() biết
    // gửi announce_peer kèm token tới đâu.
    std::mutex lastLookupMutex_;
    std::vector<std::pair<DhtContact, std::string>> lastLookupTokens_;  // (node, token)

    void receiveLoop();
    void handleIncoming(const std::string& payload, const std::string& fromIp, std::uint16_t fromPort);
    void handleQuery(const KrpcMessage& query, const std::string& fromIp, std::uint16_t fromPort);

    // Gửi 1 datagram và chờ response có transactionId khớp, timeout mặc định 2s.
    bool sendAndWait(
        const std::string& host, std::uint16_t port, const std::string& transactionId,
        const std::string& payload, KrpcMessage& outResponse,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    void sendRaw(const std::string& host, std::uint16_t port, const std::string& payload);
    static std::string makeTransactionId();

    [[nodiscard]] std::string tokenFor(const std::string& ip, std::uint16_t port) const;
};

}  // namespace p2p
