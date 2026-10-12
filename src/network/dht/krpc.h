#pragma once

#include "network/dht/node_id.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

enum class KrpcType { Query, Response, Error, Unknown };
enum class KrpcMethod { Ping, FindNode, GetPeers, AnnouncePeer, Unknown };

struct KrpcNodeInfo {
    NodeId id;
    std::string ip;
    std::uint16_t port = 0;
};

struct KrpcPeerInfo {
    std::string ip;
    std::uint16_t port = 0;
};

// 1 message KRPC đã decode (BEP 5). Không phải mọi trường đều có ý nghĩa
// cùng lúc - tuỳ type/method, xem các hàm encode tương ứng để biết trường
// nào được dùng ở đâu.
struct KrpcMessage {
    std::string transactionId;
    KrpcType type = KrpcType::Unknown;
    KrpcMethod method = KrpcMethod::Unknown;  // chỉ có nghĩa khi type == Query

    NodeId nodeId;  // id của node gửi message (query: trong "a", response: trong "r")

    // Tham số của query:
    NodeId target;            // find_node
    std::string infoHash;     // get_peers / announce_peer
    std::uint16_t port = 0;   // announce_peer
    std::string token;        // get_peers response / announce_peer query
    bool impliedPort = false; // announce_peer

    // Kết quả response:
    std::vector<KrpcNodeInfo> nodes;   // find_node, hoặc get_peers khi chưa có peer
    std::vector<KrpcPeerInfo> values;  // get_peers khi đã có peer thật

    // Lỗi:
    int errorCode = 0;
    std::string errorMessage;
};

class Krpc {
public:
    static std::string encodePing(const std::string& transactionId, const NodeId& selfId);
    static std::string encodeFindNode(const std::string& transactionId, const NodeId& selfId, const NodeId& target);
    static std::string encodeGetPeers(const std::string& transactionId, const NodeId& selfId, const std::string& infoHash);
    static std::string encodeAnnouncePeer(
        const std::string& transactionId, const NodeId& selfId, const std::string& infoHash,
        std::uint16_t port, const std::string& token, bool impliedPort = false);

    static std::string encodePingResponse(const std::string& transactionId, const NodeId& selfId);
    static std::string encodeFindNodeResponse(
        const std::string& transactionId, const NodeId& selfId, const std::vector<KrpcNodeInfo>& nodes);
    static std::string encodeGetPeersResponseWithPeers(
        const std::string& transactionId, const NodeId& selfId, const std::string& token,
        const std::vector<KrpcPeerInfo>& peers);
    static std::string encodeGetPeersResponseWithNodes(
        const std::string& transactionId, const NodeId& selfId, const std::string& token,
        const std::vector<KrpcNodeInfo>& nodes);
    static std::string encodeAnnouncePeerResponse(const std::string& transactionId, const NodeId& selfId);

    static std::string encodeError(const std::string& transactionId, int code, const std::string& message);

    static KrpcMessage decode(const std::string& raw);

    // "nodes" trong find_node/get_peers: 1 chuỗi, mỗi node 26 byte
    // (20 byte NodeId + 4 byte IPv4 + 2 byte port, big-endian).
    static std::string encodeCompactNodes(const std::vector<KrpcNodeInfo>& nodes);
    static std::vector<KrpcNodeInfo> decodeCompactNodes(const std::string& raw);
};

}  // namespace p2p
