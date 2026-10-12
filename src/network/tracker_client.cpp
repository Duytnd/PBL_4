#include "network/tracker_client.h"

#include "httplib.h"

#include <random>
#include <sstream>
#include <stdexcept>

namespace p2p {
namespace {

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

// Parser tối thiểu cho announce URL dạng "http://host[:port][/path]".
// Không hỗ trợ https:// (BEP 15 UDP tracker và HTTPS nằm ngoài phạm vi, mục 2).
ParsedUrl parseHttpUrl(const std::string& url) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        throw std::invalid_argument("TrackerClient: only http:// announce URLs are supported, got: " + url);
    }

    const std::string rest = url.substr(prefix.size());
    const std::size_t pathStart = rest.find('/');
    const std::string hostPort = pathStart == std::string::npos ? rest : rest.substr(0, pathStart);

    ParsedUrl parsed;
    parsed.path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);

    const std::size_t colon = hostPort.find(':');
    if (colon == std::string::npos) {
        parsed.host = hostPort;
    } else {
        parsed.host = hostPort.substr(0, colon);
        parsed.port = std::stoi(hostPort.substr(colon + 1));
    }

    if (parsed.host.empty()) {
        throw std::invalid_argument("TrackerClient: announce URL is missing a host: " + url);
    }

    return parsed;
}

}  // namespace

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

TrackerResponse TrackerClient::announceHttp(const std::string& announceUrl, const TrackerRequest& request) {
    const ParsedUrl url = parseHttpUrl(announceUrl);
    const std::string query = TrackerProtocol::encodeRequestQuery(request);

    httplib::Client client(url.host, url.port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);

    const httplib::Result result = client.Get(url.path + "?" + query);
    if (!result) {
        throw std::runtime_error(
            "TrackerClient: failed to reach tracker at " + announceUrl +
            " (" + httplib::to_string(result.error()) + ")");
    }
    if (result->status != 200) {
        throw std::runtime_error(
            "TrackerClient: tracker responded with HTTP " + std::to_string(result->status));
    }

    return parseAnnounceResponse(result->body);
}

}  // namespace p2p
