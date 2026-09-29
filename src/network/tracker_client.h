#pragma once

#include "network/tracker_protocol.h"

#include <cstdint>
#include <string>

namespace p2p {

class TrackerClient {
public:
    static std::string buildAnnounceRequest(const TrackerRequest& request);
    static std::string makePeerId(const std::string& prefix = "-PB0001-");
    static TrackerResponse announce(const std::string& responseBody, const TrackerRequest& request);
    static TrackerResponse parseAnnounceResponse(const std::string& responseBody);
};

}  // namespace p2p
