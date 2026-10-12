#include "network/dht/krpc.h"

#include "core/bencode.h"

#include <sstream>
#include <stdexcept>

namespace p2p {
namespace {

std::string ipv4ToCompact(const std::string& ip) {
    std::string compact(4, '\0');
    std::istringstream iss(ip);
    std::string part;
    std::size_t index = 0;

    while (std::getline(iss, part, '.')) {
        if (index >= 4 || part.empty()) {
            throw std::invalid_argument("Krpc: invalid IPv4 address");
        }
        const int value = std::stoi(part);
        if (value < 0 || value > 255) {
            throw std::invalid_argument("Krpc: IPv4 octet out of range");
        }
        compact[index++] = static_cast<char>(value);
    }
    if (index != 4) {
        throw std::invalid_argument("Krpc: IPv4 address must have 4 octets");
    }
    return compact;
}

std::string compactToIpv4(const std::string& data, std::size_t offset) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i > 0) oss << '.';
        oss << static_cast<int>(static_cast<unsigned char>(data[offset + i]));
    }
    return oss.str();
}

std::uint16_t readPortBigEndian(const std::string& data, std::size_t offset) {
    const unsigned char hi = static_cast<unsigned char>(data[offset]);
    const unsigned char lo = static_cast<unsigned char>(data[offset + 1]);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8) | lo);
}

std::string portToCompact(std::uint16_t port) {
    std::string compact(2, '\0');
    compact[0] = static_cast<char>((port >> 8) & 0xFF);
    compact[1] = static_cast<char>(port & 0xFF);
    return compact;
}

const std::string* asString(const BencodeValue& value) { return value.get_if<std::string>(); }
const BencodeDict* asDict(const BencodeValue& value) { return value.get_if<BencodeDict>(); }
const int64_t* asInt(const BencodeValue& value) { return value.get_if<int64_t>(); }
const BencodeList* asList(const BencodeValue& value) { return value.get_if<BencodeList>(); }

const char* methodName(KrpcMethod method) {
    switch (method) {
        case KrpcMethod::Ping: return "ping";
        case KrpcMethod::FindNode: return "find_node";
        case KrpcMethod::GetPeers: return "get_peers";
        case KrpcMethod::AnnouncePeer: return "announce_peer";
        default: throw std::invalid_argument("Krpc: unknown method");
    }
}

KrpcMethod parseMethodName(const std::string& name) {
    if (name == "ping") return KrpcMethod::Ping;
    if (name == "find_node") return KrpcMethod::FindNode;
    if (name == "get_peers") return KrpcMethod::GetPeers;
    if (name == "announce_peer") return KrpcMethod::AnnouncePeer;
    return KrpcMethod::Unknown;
}

std::string encodeQuery(const std::string& transactionId, KrpcMethod method, const BencodeDict& args) {
    BencodeDict root;
    root["t"] = BencodeValue(transactionId);
    root["y"] = BencodeValue(std::string("q"));
    root["q"] = BencodeValue(std::string(methodName(method)));
    root["a"] = BencodeValue(args);
    return encodeBencode(root);
}

std::string encodeResponse(const std::string& transactionId, const BencodeDict& returnValues) {
    BencodeDict root;
    root["t"] = BencodeValue(transactionId);
    root["y"] = BencodeValue(std::string("r"));
    root["r"] = BencodeValue(returnValues);
    return encodeBencode(root);
}

}  // namespace

std::string Krpc::encodeCompactNodes(const std::vector<KrpcNodeInfo>& nodes) {
    std::string compact;
    compact.reserve(nodes.size() * 26);
    for (const auto& node : nodes) {
        compact += node.id.toString();
        compact += ipv4ToCompact(node.ip);
        compact += portToCompact(node.port);
    }
    return compact;
}

std::vector<KrpcNodeInfo> Krpc::decodeCompactNodes(const std::string& raw) {
    if (raw.size() % 26 != 0) {
        throw std::invalid_argument("Krpc: compact nodes length must be a multiple of 26");
    }
    std::vector<KrpcNodeInfo> nodes;
    nodes.reserve(raw.size() / 26);
    for (std::size_t offset = 0; offset < raw.size(); offset += 26) {
        KrpcNodeInfo node;
        node.id = NodeId::fromString(raw.substr(offset, 20));
        node.ip = compactToIpv4(raw, offset + 20);
        node.port = readPortBigEndian(raw, offset + 24);
        nodes.push_back(node);
    }
    return nodes;
}

std::string Krpc::encodePing(const std::string& transactionId, const NodeId& selfId) {
    BencodeDict args;
    args["id"] = BencodeValue(selfId.toString());
    return encodeQuery(transactionId, KrpcMethod::Ping, args);
}

std::string Krpc::encodeFindNode(const std::string& transactionId, const NodeId& selfId, const NodeId& target) {
    BencodeDict args;
    args["id"] = BencodeValue(selfId.toString());
    args["target"] = BencodeValue(target.toString());
    return encodeQuery(transactionId, KrpcMethod::FindNode, args);
}

std::string Krpc::encodeGetPeers(const std::string& transactionId, const NodeId& selfId, const std::string& infoHash) {
    BencodeDict args;
    args["id"] = BencodeValue(selfId.toString());
    args["info_hash"] = BencodeValue(infoHash);
    return encodeQuery(transactionId, KrpcMethod::GetPeers, args);
}

std::string Krpc::encodeAnnouncePeer(
    const std::string& transactionId, const NodeId& selfId, const std::string& infoHash,
    std::uint16_t port, const std::string& token, bool impliedPort) {
    BencodeDict args;
    args["id"] = BencodeValue(selfId.toString());
    args["info_hash"] = BencodeValue(infoHash);
    args["port"] = BencodeValue(static_cast<int64_t>(port));
    args["token"] = BencodeValue(token);
    args["implied_port"] = BencodeValue(static_cast<int64_t>(impliedPort ? 1 : 0));
    return encodeQuery(transactionId, KrpcMethod::AnnouncePeer, args);
}

std::string Krpc::encodePingResponse(const std::string& transactionId, const NodeId& selfId) {
    BencodeDict values;
    values["id"] = BencodeValue(selfId.toString());
    return encodeResponse(transactionId, values);
}

std::string Krpc::encodeFindNodeResponse(
    const std::string& transactionId, const NodeId& selfId, const std::vector<KrpcNodeInfo>& nodes) {
    BencodeDict values;
    values["id"] = BencodeValue(selfId.toString());
    values["nodes"] = BencodeValue(encodeCompactNodes(nodes));
    return encodeResponse(transactionId, values);
}

std::string Krpc::encodeGetPeersResponseWithPeers(
    const std::string& transactionId, const NodeId& selfId, const std::string& token,
    const std::vector<KrpcPeerInfo>& peers) {
    BencodeDict values;
    values["id"] = BencodeValue(selfId.toString());
    values["token"] = BencodeValue(token);

    // "values" là 1 LIST các chuỗi 6-byte, khác "nodes" (1 chuỗi nối liền) - đúng BEP 5.
    BencodeList valuesList;
    for (const auto& peer : peers) {
        valuesList.emplace_back(ipv4ToCompact(peer.ip) + portToCompact(peer.port));
    }
    values["values"] = BencodeValue(valuesList);
    return encodeResponse(transactionId, values);
}

std::string Krpc::encodeGetPeersResponseWithNodes(
    const std::string& transactionId, const NodeId& selfId, const std::string& token,
    const std::vector<KrpcNodeInfo>& nodes) {
    BencodeDict values;
    values["id"] = BencodeValue(selfId.toString());
    values["token"] = BencodeValue(token);
    values["nodes"] = BencodeValue(encodeCompactNodes(nodes));
    return encodeResponse(transactionId, values);
}

std::string Krpc::encodeAnnouncePeerResponse(const std::string& transactionId, const NodeId& selfId) {
    BencodeDict values;
    values["id"] = BencodeValue(selfId.toString());
    return encodeResponse(transactionId, values);
}

std::string Krpc::encodeError(const std::string& transactionId, int code, const std::string& message) {
    BencodeDict root;
    root["t"] = BencodeValue(transactionId);
    root["y"] = BencodeValue(std::string("e"));
    BencodeList error;
    error.emplace_back(static_cast<int64_t>(code));
    error.emplace_back(message);
    root["e"] = BencodeValue(error);
    return encodeBencode(root);
}

KrpcMessage Krpc::decode(const std::string& raw) {
    const auto decoded = decodeBencode(raw);
    const auto* root = asDict(decoded);
    if (root == nullptr) {
        throw std::invalid_argument("Krpc: message must be a dictionary");
    }

    KrpcMessage message;

    const auto tIt = root->find("t");
    if (tIt != root->end()) {
        const auto* t = asString(tIt->second);
        if (t != nullptr) {
            message.transactionId = *t;
        }
    }

    const auto yIt = root->find("y");
    if (yIt == root->end()) {
        throw std::invalid_argument("Krpc: missing 'y' (message type)");
    }
    const auto* y = asString(yIt->second);
    if (y == nullptr) {
        throw std::invalid_argument("Krpc: 'y' must be a string");
    }

    if (*y == "q") {
        message.type = KrpcType::Query;

        const auto qIt = root->find("q");
        if (qIt == root->end() || asString(qIt->second) == nullptr) {
            throw std::invalid_argument("Krpc: query missing 'q'");
        }
        message.method = parseMethodName(*asString(qIt->second));

        const auto aIt = root->find("a");
        if (aIt == root->end() || asDict(aIt->second) == nullptr) {
            throw std::invalid_argument("Krpc: query missing 'a'");
        }
        const auto& args = *asDict(aIt->second);

        const auto idIt = args.find("id");
        if (idIt == args.end() || asString(idIt->second) == nullptr) {
            throw std::invalid_argument("Krpc: query missing 'a.id'");
        }
        message.nodeId = NodeId::fromString(*asString(idIt->second));

        if (message.method == KrpcMethod::FindNode) {
            const auto targetIt = args.find("target");
            if (targetIt == args.end() || asString(targetIt->second) == nullptr) {
                throw std::invalid_argument("Krpc: find_node missing 'target'");
            }
            message.target = NodeId::fromString(*asString(targetIt->second));
        } else if (message.method == KrpcMethod::GetPeers) {
            const auto infoHashIt = args.find("info_hash");
            if (infoHashIt == args.end() || asString(infoHashIt->second) == nullptr) {
                throw std::invalid_argument("Krpc: get_peers missing 'info_hash'");
            }
            message.infoHash = *asString(infoHashIt->second);
        } else if (message.method == KrpcMethod::AnnouncePeer) {
            const auto infoHashIt = args.find("info_hash");
            const auto portIt = args.find("port");
            const auto tokenIt = args.find("token");
            if (infoHashIt == args.end() || asString(infoHashIt->second) == nullptr ||
                portIt == args.end() || asInt(portIt->second) == nullptr ||
                tokenIt == args.end() || asString(tokenIt->second) == nullptr) {
                throw std::invalid_argument("Krpc: announce_peer missing required field");
            }
            message.infoHash = *asString(infoHashIt->second);
            message.port = static_cast<std::uint16_t>(*asInt(portIt->second));
            message.token = *asString(tokenIt->second);

            const auto impliedIt = args.find("implied_port");
            if (impliedIt != args.end() && asInt(impliedIt->second) != nullptr) {
                message.impliedPort = (*asInt(impliedIt->second)) != 0;
            }
        }
    } else if (*y == "r") {
        message.type = KrpcType::Response;

        const auto rIt = root->find("r");
        if (rIt == root->end() || asDict(rIt->second) == nullptr) {
            throw std::invalid_argument("Krpc: response missing 'r'");
        }
        const auto& values = *asDict(rIt->second);

        const auto idIt = values.find("id");
        if (idIt == values.end() || asString(idIt->second) == nullptr) {
            throw std::invalid_argument("Krpc: response missing 'r.id'");
        }
        message.nodeId = NodeId::fromString(*asString(idIt->second));

        const auto tokenIt = values.find("token");
        if (tokenIt != values.end() && asString(tokenIt->second) != nullptr) {
            message.token = *asString(tokenIt->second);
        }

        const auto nodesIt = values.find("nodes");
        if (nodesIt != values.end() && asString(nodesIt->second) != nullptr) {
            message.nodes = decodeCompactNodes(*asString(nodesIt->second));
        }

        const auto valuesIt = values.find("values");
        if (valuesIt != values.end() && asList(valuesIt->second) != nullptr) {
            for (const auto& entry : *asList(valuesIt->second)) {
                const auto* compact = asString(entry);
                if (compact == nullptr || compact->size() != 6) {
                    continue;
                }
                KrpcPeerInfo peer;
                peer.ip = compactToIpv4(*compact, 0);
                peer.port = readPortBigEndian(*compact, 4);
                message.values.push_back(peer);
            }
        }
    } else if (*y == "e") {
        message.type = KrpcType::Error;

        const auto eIt = root->find("e");
        if (eIt == root->end() || asList(eIt->second) == nullptr || asList(eIt->second)->size() != 2) {
            throw std::invalid_argument("Krpc: error missing 'e' [code, message]");
        }
        const auto& errorList = *asList(eIt->second);
        const auto* code = asInt(errorList[0]);
        const auto* text = asString(errorList[1]);
        if (code == nullptr || text == nullptr) {
            throw std::invalid_argument("Krpc: malformed error fields");
        }
        message.errorCode = static_cast<int>(*code);
        message.errorMessage = *text;
    } else {
        throw std::invalid_argument("Krpc: unknown message type 'y'");
    }

    return message;
}

}  // namespace p2p
