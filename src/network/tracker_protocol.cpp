#include "network/tracker_protocol.h"

#include <arpa/inet.h>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace p2p {
namespace {

std::string percentEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return encoded;
}

std::string ipToCompact(const std::string& ip) {
    std::string compact(4, '\0');
    std::istringstream iss(ip);
    std::string part;
    std::size_t index = 0;

    while (std::getline(iss, part, '.')) {
        if (index >= 4 || part.empty()) {
            throw std::invalid_argument("Invalid IPv4 address for compact peer encoding");
        }
        const int value = std::stoi(part);
        if (value < 0 || value > 255) {
            throw std::invalid_argument("IPv4 octet out of range");
        }
        compact[index++] = static_cast<char>(value);
    }

    if (index != 4) {
        throw std::invalid_argument("IPv4 address must contain four octets");
    }

    return compact;
}

std::string compactToIp(const std::string& compact, std::size_t index) {
    if (compact.size() < index + 4) {
        throw std::invalid_argument("Compact peer data is truncated");
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i > 0) {
            oss << '.';
        }
        const unsigned char byte = static_cast<unsigned char>(compact[index + i]);
        oss << static_cast<int>(byte);
    }
    return oss.str();
}

std::uint16_t readPortBigEndian(const std::string& compact, std::size_t index) {
    if (compact.size() < index + 2) {
        throw std::invalid_argument("Compact peer port is truncated");
    }

    const unsigned char hi = static_cast<unsigned char>(compact[index]);
    const unsigned char lo = static_cast<unsigned char>(compact[index + 1]);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(hi) << 8) | static_cast<std::uint16_t>(lo));
}

const std::string* asString(const BencodeValue& value) {
    return value.get_if<std::string>();
}

const BencodeDict* asDict(const BencodeValue& value) {
    return value.get_if<BencodeDict>();
}

const int64_t* asInt(const BencodeValue& value) {
    return value.get_if<int64_t>();
}

const BencodeList* asList(const BencodeValue& value) {
    return value.get_if<BencodeList>();
}

}  // namespace

std::string TrackerProtocol::encodeRequestQuery(const TrackerRequest& request) {
    std::ostringstream query;
    query << "info_hash=" << percentEncode(request.infoHash)
          << "&peer_id=" << percentEncode(request.peerId)
          << "&port=" << request.port
          << "&uploaded=" << request.uploaded
          << "&downloaded=" << request.downloaded
          << "&left=" << request.left
          << "&compact=" << (request.compact ? 1 : 0);

    if (!request.event.empty()) {
        query << "&event=" << request.event;
    }
    if (!request.ip.empty()) {
        query << "&ip=" << percentEncode(request.ip);
    }
    if (request.numwant > 0) {
        query << "&numwant=" << request.numwant;
    }

    return query.str();
}

std::string TrackerProtocol::encodeCompactPeers(const std::vector<TrackerPeer>& peers) {
    std::string compact;
    compact.reserve(peers.size() * 6);

    for (const auto& peer : peers) {
        const std::string ipBytes = ipToCompact(peer.ip);
        if (ipBytes.size() != 4) {
            throw std::invalid_argument("Peer IP must be IPv4");
        }
        compact.append(ipBytes);

        const std::uint16_t networkPort = htons(peer.port);
        compact.append(reinterpret_cast<const char*>(&networkPort), sizeof(networkPort));
    }

    return compact;
}

std::vector<TrackerPeer> TrackerProtocol::decodeCompactPeers(const std::string& compactPeers) {
    if (compactPeers.empty()) {
        return {};
    }

    if (compactPeers.size() % 6 != 0) {
        throw std::invalid_argument("Compact peers length is not a multiple of 6");
    }

    std::vector<TrackerPeer> peers;
    peers.reserve(compactPeers.size() / 6);

    for (std::size_t index = 0; index < compactPeers.size(); index += 6) {
        TrackerPeer peer;
        peer.ip = compactToIp(compactPeers, index);
        const std::uint16_t portValue = readPortBigEndian(compactPeers, index + 4);
        peer.port = portValue;
        peers.push_back(peer);
    }

    return peers;
}

std::string TrackerProtocol::encodeSuccessResponse(int interval, const std::vector<TrackerPeer>& peers) {
    BencodeDict response;
    response["interval"] = BencodeValue(static_cast<int64_t>(interval));
    response["peers"] = BencodeValue(encodeCompactPeers(peers));
    return encodeBencode(response);
}

std::string TrackerProtocol::encodeFailureResponse(const std::string& reason) {
    BencodeDict response;
    response["failure reason"] = BencodeValue(reason);
    return encodeBencode(response);
}

TrackerResponse TrackerProtocol::decodeResponse(const std::string& bencodedResponse) {
    const auto decoded = decodeBencode(bencodedResponse);
    const auto* root = asDict(decoded);
    if (root == nullptr) {
        throw std::invalid_argument("Tracker response must be a dictionary");
    }

    TrackerResponse response;

    const auto failureIt = root->find("failure reason");
    if (failureIt != root->end()) {
        const auto* reason = asString(failureIt->second);
        if (reason == nullptr) {
            throw std::invalid_argument("Tracker failure reason must be a string");
        }
        response.success = false;
        response.failureReason = *reason;
        return response;
    }

    response.success = true;

    const auto intervalIt = root->find("interval");
    if (intervalIt != root->end()) {
        const auto* intervalValue = asInt(intervalIt->second);
        if (intervalValue != nullptr) {
            response.interval = static_cast<int>(*intervalValue);
        }
    }

    const auto peersIt = root->find("peers");
    if (peersIt != root->end()) {
        const auto* peersString = asString(peersIt->second);
        if (peersString != nullptr) {
            response.peers = decodeCompactPeers(*peersString);
        } else {
            const auto* peersList = asList(peersIt->second);
            if (peersList != nullptr) {
                for (const auto& entry : *peersList) {
                    const auto* dict = asDict(entry);
                    if (dict == nullptr) {
                        continue;
                    }
                    auto ipIt = dict->find("ip");
                    auto portIt = dict->find("port");
                    if (ipIt == dict->end() || portIt == dict->end()) {
                        continue;
                    }
                    const auto* ipValue = asString(ipIt->second);
                    const auto* portValue = asInt(portIt->second);
                    if (ipValue != nullptr && portValue != nullptr) {
                        response.peers.push_back({*ipValue, static_cast<std::uint16_t>(*portValue)});
                    }
                }
            }
        }
    }

    return response;
}

}  // namespace p2p
