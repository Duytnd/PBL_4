#pragma once

#include "core/bencode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct TrackerPeer {
    std::string ip;
    std::uint16_t port = 0;
};

struct TrackerRequest {
    std::string infoHash;
    std::string peerId;
    std::string ip;
    std::uint16_t port = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t downloaded = 0;
    std::uint64_t left = 0;
    std::string event;
    int numwant = 50;
    bool compact = true;
};

struct TrackerResponse {
    bool success = false;
    int interval = 0;
    std::vector<TrackerPeer> peers;
    std::string failureReason;
};

class TrackerProtocol {
public:
    static std::string encodeRequestQuery(const TrackerRequest& request);
    static std::string encodeCompactPeers(const std::vector<TrackerPeer>& peers);
    static std::vector<TrackerPeer> decodeCompactPeers(const std::string& compactPeers);
    static std::string encodeSuccessResponse(int interval, const std::vector<TrackerPeer>& peers);
    static std::string encodeFailureResponse(const std::string& reason);
    static TrackerResponse decodeResponse(const std::string& bencodedResponse);
};

}  // namespace p2p
