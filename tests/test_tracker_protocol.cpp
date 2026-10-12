#include "test_framework.h"

#include "network/tracker_client.h"
#include "network/tracker_protocol.h"
#include "tracker/peer_registry.h"
#include "tracker/tracker_server.h"

using namespace p2p;

namespace {

// peer_id theo chuẩn BitTorrent luôn đúng 20 byte; đệm thêm '0' cho đủ độ dài
// để test dễ đọc (vd "peerA" -> "peerA000000000000000").
std::string pad20(const std::string& label) {
    std::string padded = label;
    padded.resize(20, '0');
    return padded;
}

TrackerRequest makeRequest(const std::string& peerIdLabel, std::uint16_t port, const std::string& ip = "") {
    TrackerRequest request;
    request.infoHash = std::string(20, 'h');
    request.peerId = pad20(peerIdLabel);
    request.port = port;
    request.ip = ip;
    request.left = 100;
    return request;
}

}  // namespace

void testEncodeRequestQueryPercentEncodesBinaryBytes() {
    TrackerRequest request = makeRequest(std::string(20, '\x01'), 6881);
    request.infoHash = std::string("\x00\x01\xff", 3);

    const std::string query = TrackerProtocol::encodeRequestQuery(request);
    CHECK(query.find("info_hash=%00%01%FF") != std::string::npos);
    CHECK(query.find("port=6881") != std::string::npos);
    CHECK(query.find("compact=1") != std::string::npos);
}

void testCompactPeersRoundTrip() {
    const std::vector<TrackerPeer> peers = {{"192.168.1.10", 6881}, {"10.0.0.1", 51413}};
    const std::string compact = TrackerProtocol::encodeCompactPeers(peers);
    CHECK_EQ(compact.size(), std::size_t(12));  // 2 peer x 6 byte

    const auto decoded = TrackerProtocol::decodeCompactPeers(compact);
    CHECK_EQ(decoded.size(), std::size_t(2));
    CHECK_EQ(decoded[0].ip, std::string("192.168.1.10"));
    CHECK_EQ(decoded[0].port, std::uint16_t(6881));
    CHECK_EQ(decoded[1].ip, std::string("10.0.0.1"));
    CHECK_EQ(decoded[1].port, std::uint16_t(51413));
}

void testDecodeCompactPeersRejectsBadLength() {
    CHECK_THROWS(TrackerProtocol::decodeCompactPeers("12345"));  // không chia hết 6
}

void testSuccessResponseRoundTrip() {
    const std::vector<TrackerPeer> peers = {{"1.2.3.4", 1000}};
    const std::string response = TrackerProtocol::encodeSuccessResponse(1800, peers);

    const TrackerResponse decoded = TrackerProtocol::decodeResponse(response);
    CHECK(decoded.success);
    CHECK_EQ(decoded.interval, 1800);
    CHECK_EQ(decoded.peers.size(), std::size_t(1));
    CHECK_EQ(decoded.peers[0].ip, std::string("1.2.3.4"));
}

void testFailureResponseRoundTrip() {
    const std::string response = TrackerProtocol::encodeFailureResponse("bad request");
    const TrackerResponse decoded = TrackerProtocol::decodeResponse(response);
    CHECK(!decoded.success);
    CHECK_EQ(decoded.failureReason, std::string("bad request"));
}

void testTrackerClientMakePeerIdHasPrefixAndLength() {
    const std::string peerId = TrackerClient::makePeerId("-PB0001-");
    CHECK_EQ(peerId.size(), std::size_t(20));  // 8 byte prefix + 12 byte ngẫu nhiên
    CHECK_EQ(peerId.substr(0, 8), std::string("-PB0001-"));
}

void testPeerRegistryUpsertAndGetPeersExcludesSelf() {
    PeerRegistry registry(/*expirySeconds=*/3600, /*maxPeersPerTorrent=*/50);
    const std::string infoHash(20, 'h');

    registry.upsert(infoHash, {"peerA", "10.0.0.1", 6881, 0, 0, 0});
    registry.upsert(infoHash, {"peerB", "10.0.0.2", 6882, 0, 0, 0});

    const auto peersForA = registry.getPeers(infoHash, "peerA");
    CHECK_EQ(peersForA.size(), std::size_t(1));
    CHECK_EQ(peersForA[0].ip, std::string("10.0.0.2"));

    CHECK(registry.hasPeer(infoHash, "peerA"));
    CHECK(!registry.hasPeer(infoHash, "peerX"));
}

void testPeerRegistryExpiresOldPeers() {
    PeerRegistry registry(/*expirySeconds=*/10, /*maxPeersPerTorrent=*/50);
    const std::string infoHash(20, 'h');
    const auto now = PeerRegistry::Clock::now();

    registry.upsert(infoHash, {"peerA", "10.0.0.1", 6881, 0, 0, 0}, now);

    // Hỏi lại ngay: vẫn còn trong danh sách.
    CHECK_EQ(registry.getPeers(infoHash, "", now).size(), std::size_t(1));

    // 20s sau (> 10s hết hạn), không announce lại -> bị coi là đã rời mạng.
    const auto later = now + std::chrono::seconds(20);
    CHECK_EQ(registry.getPeers(infoHash, "", later).size(), std::size_t(0));

    registry.purgeExpired(later);
    CHECK_EQ(registry.peerCount(infoHash), std::size_t(0));
}

void testPeerRegistryEvictsOldestWhenFull() {
    PeerRegistry registry(/*expirySeconds=*/3600, /*maxPeersPerTorrent=*/2);
    const std::string infoHash(20, 'h');
    const auto now = PeerRegistry::Clock::now();

    registry.upsert(infoHash, {"peerA", "10.0.0.1", 1, 0, 0, 0}, now);
    registry.upsert(infoHash, {"peerB", "10.0.0.2", 2, 0, 0, 0}, now + std::chrono::seconds(1));
    CHECK_EQ(registry.peerCount(infoHash), std::size_t(2));

    // Đầy chỗ (max 2): thêm peerC phải đẩy peer cũ nhất (peerA) ra.
    registry.upsert(infoHash, {"peerC", "10.0.0.3", 3, 0, 0, 0}, now + std::chrono::seconds(2));
    CHECK_EQ(registry.peerCount(infoHash), std::size_t(2));
    CHECK(!registry.hasPeer(infoHash, "peerA"));
    CHECK(registry.hasPeer(infoHash, "peerB"));
    CHECK(registry.hasPeer(infoHash, "peerC"));
}

void testTrackerServerAnnounceReturnsOtherPeersOnly() {
    TrackerServer tracker(1800, 50);

    const std::string response1 = tracker.announce(makeRequest("peerA", 6881, "10.0.0.1"));
    const TrackerResponse decoded1 = TrackerClient::parseAnnounceResponse(response1);
    CHECK(decoded1.success);
    CHECK(decoded1.peers.empty());  // peerA là peer đầu tiên, chưa ai khác trong mạng

    const std::string response2 = tracker.announce(makeRequest("peerB", 6882, "10.0.0.2"));
    const TrackerResponse decoded2 = TrackerClient::parseAnnounceResponse(response2);
    CHECK_EQ(decoded2.peers.size(), std::size_t(1));
    if (decoded2.peers.size() == 1) {
        CHECK_EQ(decoded2.peers[0].ip, std::string("10.0.0.1"));  // thấy peerA, không thấy chính mình
    }
}

void testTrackerServerStoppedEventRemovesPeer() {
    TrackerServer tracker(1800, 50);
    const std::string infoHash(20, 'h');

    tracker.announce(makeRequest("peerA", 6881, "10.0.0.1"));
    CHECK(tracker.hasPeer(infoHash, pad20("peerA")));

    TrackerRequest stopRequest = makeRequest("peerA", 6881, "10.0.0.1");
    stopRequest.event = "stopped";
    tracker.announce(stopRequest);

    CHECK(!tracker.hasPeer(infoHash, pad20("peerA")));
}

void testTrackerServerRejectsMalformedInfoHashOrPeerId() {
    TrackerServer tracker(1800, 50);

    TrackerRequest badInfoHash = makeRequest("peerA", 6881);
    badInfoHash.infoHash = "too-short";
    const TrackerResponse decoded = TrackerClient::parseAnnounceResponse(tracker.announce(badInfoHash));
    CHECK(!decoded.success);

    TrackerRequest badPeerId = makeRequest("peerA", 6881);
    badPeerId.peerId = "short";  // cố ý không đủ 20 byte, không qua pad20()
    const TrackerResponse decoded2 = TrackerClient::parseAnnounceResponse(tracker.announce(badPeerId));
    CHECK(!decoded2.success);
}

int main() {
    RUN(testEncodeRequestQueryPercentEncodesBinaryBytes);
    RUN(testCompactPeersRoundTrip);
    RUN(testDecodeCompactPeersRejectsBadLength);
    RUN(testSuccessResponseRoundTrip);
    RUN(testFailureResponseRoundTrip);
    RUN(testTrackerClientMakePeerIdHasPrefixAndLength);
    RUN(testPeerRegistryUpsertAndGetPeersExcludesSelf);
    RUN(testPeerRegistryExpiresOldPeers);
    RUN(testPeerRegistryEvictsOldestWhenFull);
    RUN(testTrackerServerAnnounceReturnsOtherPeersOnly);
    RUN(testTrackerServerStoppedEventRemovesPeer);
    RUN(testTrackerServerRejectsMalformedInfoHashOrPeerId);
    return TEST_REPORT();
}
