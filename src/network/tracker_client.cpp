#include "network/tracker_client.h"

#include <random>
#include <sstream>

namespace p2p {

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

}  // namespace p2p
