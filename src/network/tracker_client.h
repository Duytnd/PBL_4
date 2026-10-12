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

    // Gửi GET announce thật tới tracker qua HTTP (BEP 3) và parse response.
    // announceUrl dạng "http://host:port/path", chỉ hỗ trợ HTTP thường (không
    // UDP tracker, không HTTPS - nằm ngoài phạm vi kế hoạch mục 2).
    // Ném std::runtime_error nếu không kết nối được hoặc HTTP status != 200.
    static TrackerResponse announceHttp(const std::string& announceUrl, const TrackerRequest& request);
};

}  // namespace p2p
