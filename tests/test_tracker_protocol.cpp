#include "test_framework.h"
#include "test_https_certs.h"

#include "httplib.h"

#include "network/tracker_client.h"
#include "network/tracker_protocol.h"
#include "network/udp_rendezvous.h"
#include "tracker/endpoint_rate_limiter.h"
#include "tracker/peer_registry.h"
#include "tracker/tracker_server.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

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

void testUdpRendezvousRegisterAndPunchRequestCodec() {
    const std::string infoHash(20, 'h');
    const std::string peerId(20, 'p');
    const auto registration = network::udp_rendezvous::encode_register_request(infoHash, peerId, 6881);
    CHECK_EQ(registration.size(), std::size_t(56));  // 48 + khoá phiên 8 byte

    network::udp_rendezvous::Request decoded;
    CHECK(network::udp_rendezvous::decode_request(registration.data(), registration.size(), decoded));
    CHECK_EQ(static_cast<int>(decoded.opcode),
             static_cast<int>(network::udp_rendezvous::Opcode::Register));
    CHECK_EQ(decoded.info_hash, infoHash);
    CHECK_EQ(decoded.peer_id, peerId);
    CHECK_EQ(decoded.peer_tcp_port, std::uint16_t(6881));

    const auto punch = network::udp_rendezvous::encode_punch_request(
        infoHash, peerId, 6881, "198.51.100.20", 6882, 0x12345678);
    CHECK_EQ(punch.size(), std::size_t(66));  // 58 + khoá phiên 8 byte
    CHECK(network::udp_rendezvous::decode_request(punch.data(), punch.size(), decoded));
    CHECK_EQ(static_cast<int>(decoded.opcode),
             static_cast<int>(network::udp_rendezvous::Opcode::Punch));
    CHECK_EQ(decoded.peer_tcp_port, std::uint16_t(6881));
    CHECK_EQ(decoded.target_ip, std::string("198.51.100.20"));
    CHECK_EQ(decoded.target_tcp_port, std::uint16_t(6882));
    CHECK_EQ(decoded.transaction_id, std::uint32_t(0x12345678));
    CHECK(!network::udp_rendezvous::decode_request(punch.data(), punch.size() - 1, decoded));
    CHECK_THROWS(network::udp_rendezvous::encode_punch_request(
        infoHash, peerId, 6881, "not-an-ip", 6882, 1));
}

void testUdpRendezvousResultsAreBoundedAndRoundTrip() {
    const std::string infoHash(20, 'h');
    const auto request = network::udp_rendezvous::encode_punch_request(
        infoHash, std::string(20, 'p'), 6880, "203.0.113.8", 6881, 42);
    const auto response = network::udp_rendezvous::encode_punch_result(
        network::udp_rendezvous::Status::Ok, infoHash, 42, "198.51.100.20", 52000);
    CHECK(response.size() < request.size());

    network::udp_rendezvous::Result decoded;
    CHECK(network::udp_rendezvous::decode_punch_result(response.data(), response.size(), decoded));
    CHECK_EQ(decoded.info_hash, infoHash);
    CHECK_EQ(decoded.transaction_id, std::uint32_t(42));
    CHECK_EQ(decoded.ip, std::string("198.51.100.20"));
    CHECK_EQ(decoded.port, std::uint16_t(52000));

    const auto notification = network::udp_rendezvous::encode_notification(
        infoHash, "203.0.113.8", 6880, "203.0.113.8", 41000);
    CHECK(notification.size() < request.size());
    network::udp_rendezvous::Notification decodedNotification;
    CHECK(network::udp_rendezvous::decode_notification(
        notification.data(), notification.size(), decodedNotification));
    CHECK_EQ(decodedNotification.tcp_ip, std::string("203.0.113.8"));
    CHECK_EQ(decodedNotification.tcp_port, std::uint16_t(6880));
    CHECK_EQ(decodedNotification.ip, std::string("203.0.113.8"));
    CHECK_EQ(decodedNotification.port, std::uint16_t(41000));
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

void testTrackerClientExtractsUdpRendezvousEndpoint() {
    const auto endpoint = TrackerClient::udpRendezvousEndpoint(
        "http://tracker.example:8080/announce");
    CHECK_EQ(endpoint.first, std::string("tracker.example"));
    CHECK_EQ(endpoint.second, std::uint16_t(8080));
}

void testTrackerClientRetriesTransientHttpFailures() {
    httplib::Server server;
    const int port = server.bind_to_any_port("127.0.0.1");
    std::atomic<int> attempts{0};

    server.Get("/announce", [&](const httplib::Request&, httplib::Response& res) {
        if (attempts.fetch_add(1) == 0) {
            res.status = 503;
            res.set_content("temporary tracker outage", "text/plain");
            return;
        }
        res.set_content(TrackerProtocol::encodeSuccessResponse(1800, {}), "text/plain");
    });

    std::thread serverThread([&]() {
        server.listen_after_bind();
    });

    TrackerRequest request;
    request.infoHash = std::string(20, 'a');
    request.peerId = pad20("peerA");
    request.port = 6881;
    request.left = 100;

    const TrackerResponse resp = TrackerClient::announceHttp("http://127.0.0.1:" + std::to_string(port) + "/announce", request);
    CHECK(resp.success);
    CHECK_EQ(attempts.load(), 2);

    server.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }
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

void testEndpointRateLimiterIsBoundedAndFailsClosed() {
    using Limiter = EndpointRateLimiter;
    const auto t0 = Limiter::Clock::now();
    Limiter limiter(std::chrono::milliseconds(25), /*maxEntries=*/3, std::chrono::milliseconds(1000));

    CHECK(limiter.allow("a", t0));
    CHECK(!limiter.allow("a", t0 + std::chrono::milliseconds(10)));   // quá nhanh
    CHECK(limiter.allow("a", t0 + std::chrono::milliseconds(30)));    // đủ giãn cách

    CHECK(limiter.allow("b", t0));
    CHECK(limiter.allow("c", t0));
    CHECK_EQ(limiter.size(), std::size_t(3));
    // Đầy mà chưa mục nào cũ: khoá mới bị từ chối, bộ nhớ KHÔNG tăng (trước đây map phình vô hạn).
    CHECK(!limiter.allow("d", t0 + std::chrono::milliseconds(100)));
    CHECK_EQ(limiter.size(), std::size_t(3));
    // Khoá đã biết vẫn được xử lý bình thường khi bảng đầy.
    CHECK(limiter.allow("b", t0 + std::chrono::milliseconds(200)));
    // Sau khi các mục cũ quá hạn thì dọn và nhận khoá mới.
    CHECK(limiter.allow("d", t0 + std::chrono::milliseconds(2500)));
    CHECK(limiter.size() <= std::size_t(3));
}

void testEndpointRateLimiterSurvivesFloodOfDistinctSources() {
    EndpointRateLimiter limiter(std::chrono::milliseconds(25), /*maxEntries=*/1000, std::chrono::milliseconds(1000));
    const auto t0 = EndpointRateLimiter::Clock::now();
    std::size_t accepted = 0;
    for (int i = 0; i < 200000; ++i) {
        accepted += limiter.allow("10.0.0." + std::to_string(i % 250) + ":" + std::to_string(i), t0) ? 1 : 0;
    }
    CHECK(limiter.size() <= std::size_t(1000));  // 200.000 nguồn khác nhau, bộ nhớ vẫn bị chặn
    CHECK(accepted <= std::size_t(1000));
}

void testPeerRegistryOneIpCannotEvictOtherIps() {
    PeerRegistry registry(/*expiry=*/3600, /*maxPeersPerTorrent=*/6, /*maxSwarms=*/100,
                          /*maxPeersPerIpPerSwarm=*/2, /*maxSwarmsPerIp=*/100);
    const std::string infoHash(20, 'h');
    const auto now = PeerRegistry::Clock::now();

    registry.upsert(infoHash, {"honestA", "10.0.0.1", 1000, 0, 0, 0}, now);
    registry.upsert(infoHash, {"honestB", "10.0.0.2", 1000, 0, 0, 0}, now + std::chrono::seconds(1));

    // Kẻ tấn công từ 1 IP thay peer_id liên tục.
    for (int i = 0; i < 100; ++i) {
        registry.upsert(infoHash, {"evil" + std::to_string(i), "203.0.113.66", static_cast<std::uint16_t>(2000 + i), 0, 0, 0},
                        now + std::chrono::seconds(2 + i));
    }
    CHECK(registry.hasPeer(infoHash, "honestA"));   // peer thật vẫn còn (trước đây bị đẩy ra vì "cũ nhất")
    CHECK(registry.hasPeer(infoHash, "honestB"));
    CHECK_EQ(registry.peerCount(infoHash), std::size_t(4));  // 2 peer thật + tối đa 2 của kẻ tấn công
    CHECK(registry.hasPeer(infoHash, "evil99"));    // peer mới nhất của IP đó được giữ
    CHECK(!registry.hasPeer(infoHash, "evil0"));
}

void testPeerRegistryOneIpCannotFillSwarmTable() {
    PeerRegistry registry(/*expiry=*/3600, /*maxPeersPerTorrent=*/50, /*maxSwarms=*/100,
                          /*maxPeersPerIpPerSwarm=*/10, /*maxSwarmsPerIp=*/5);
    const auto now = PeerRegistry::Clock::now();
    // Bảng chưa đầy nửa: chưa áp giới hạn theo IP (không làm khó trường hợp bình thường).
    for (int i = 0; i < 50; ++i) {
        CHECK(registry.upsert(std::string(20, static_cast<char>('A' + (i % 26))) + std::to_string(i) , {"p" + std::to_string(i), "203.0.113.66", 1, 0, 0, 0}, now));
    }
    // Bảng đã đầy nửa (50/100): IP đó đã vượt 5 swarm nên bị từ chối tạo swarm mới.
    CHECK(!registry.upsert("another-info-hash-0001", {"px", "203.0.113.66", 1, 0, 0, 0}, now));
    // IP khác vẫn tạo được swarm.
    CHECK(registry.upsert("another-info-hash-0002", {"py", "198.51.100.7", 1, 0, 0, 0}, now));
    // IP đó vẫn được vào swarm đã tồn tại của người khác.
    CHECK(registry.upsert("another-info-hash-0002", {"pz", "203.0.113.66", 2, 0, 0, 0}, now));
}

void testPeerRegistryLanWithSeveralClientsOnOneIpStillWorks() {
    // Nhiều client/phiên cùng 1 máy (demo LAN) vẫn nằm trong giới hạn mặc định.
    PeerRegistry registry(/*expiry=*/3600, /*maxPeersPerTorrent=*/50);
    const std::string infoHash(20, 'h');
    for (int i = 0; i < 8; ++i) {
        registry.upsert(infoHash, {"local" + std::to_string(i), "192.168.1.232", static_cast<std::uint16_t>(6881 + i), 0, 0, 0});
    }
    CHECK_EQ(registry.peerCount(infoHash), std::size_t(8));
}

void testPeerRegistryRejectsNewSwarmWhenFull() {
    PeerRegistry registry(3600, 50, /*maxSwarms=*/1);
    CHECK(registry.upsert(std::string(20, 'a'), {"peerA", "10.0.0.1", 1, 0, 0, 0}));
    CHECK(!registry.upsert(std::string(20, 'b'), {"peerB", "10.0.0.2", 2, 0, 0, 0}));
    // Swarm đã có vẫn nhận peer mới.
    CHECK(registry.upsert(std::string(20, 'a'), {"peerC", "10.0.0.3", 3, 0, 0, 0}));
}

void testPeerRegistryRendezvousUsesFreshObservedUdpEndpoints() {
    PeerRegistry registry(/*expirySeconds=*/3600, /*maxPeersPerTorrent=*/50);
    const std::string infoHash(20, 'h');
    const auto now = PeerRegistry::Clock::now();

    registry.upsert(infoHash, {"peerA", "203.0.113.10", 6881, 0, 0, 0}, now);
    registry.upsert(infoHash, {"peerB", "198.51.100.20", 6882, 0, 0, 0}, now);
    CHECK(registry.updateUdpEndpoint(infoHash, "peerA", "203.0.113.10", 41000, 6881, now));
    CHECK(registry.updateUdpEndpoint(infoHash, "peerB", "198.51.100.20", 52000, 6882, now));

    const auto endpoint = registry.getUdpEndpointByTcpEndpoint(
        infoHash, "peerA", "203.0.113.10", "198.51.100.20", 6882, now);
    CHECK(endpoint.has_value());
    if (endpoint) {
        CHECK_EQ(endpoint->ip, std::string("198.51.100.20"));
        CHECK_EQ(endpoint->port, std::uint16_t(52000));
    }

    CHECK(!registry.updateUdpEndpoint(infoHash, "peerA", "192.0.2.55", 41000, 6881, now));
    CHECK(!registry.getUdpEndpointByTcpEndpoint(
        infoHash, "unknown", "203.0.113.10", "198.51.100.20", 6882, now));
    CHECK(!registry.getUdpEndpointByTcpEndpoint(
        infoHash, "peerA", "203.0.113.10", "198.51.100.20", 6882,
        now + std::chrono::seconds(121)));
}

void testTrackerServerRejectsIpv6Peer() {
    TrackerServer server(1800, 50);
    TrackerRequest request;
    request.infoHash = std::string(20, 'h');
    request.peerId = std::string(20, 'p');
    request.port = 6881;
    request.ip = "::1";
    const auto response = TrackerProtocol::decodeResponse(server.announce(request));
    CHECK(!response.failureReason.empty());
    CHECK(!server.hasPeer(request.infoHash, request.peerId));
}

// ---- UDP tracker (BEP 15) ----

namespace {

void be32(std::string& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((v >> s) & 0xFF));
}

std::uint32_t read32(const std::string& in, std::size_t pos) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(in[pos + i]);
    return v;
}

TrackerRequest makeUdpRequest() {
    TrackerRequest request;
    request.infoHash = std::string(20, 'h');
    request.peerId = pad20("udp-peer");
    request.port = 6881;
    request.left = 1000;
    request.event = "started";
    return request;
}

// Tracker UDP giả trên loopback: trả lời connect rồi announce (hoặc lỗi nếu failAnnounce).
class FakeUdpTracker {
public:
    explicit FakeUdpTracker(bool failAnnounce) : failAnnounce_(failAnnounce) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
#ifdef _WIN32
        const DWORD timeout = 3000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval tv{3, 0};
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeUdpTracker() {
        thread_.join();
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
    }
    std::uint16_t port() const { return port_; }
    std::string lastAnnounce;  // gói announce nhận được (đọc sau khi announce xong)

private:
    void serve() {
        for (int handled = 0; handled < 2;) {
            char buf[512];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            const int n = ::recvfrom(socket_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 16) return;  // timeout / gói rác
            const std::string packet(buf, static_cast<std::size_t>(n));
            const std::uint32_t action = read32(packet, 8);
            const std::uint32_t tx = read32(packet, 12);
            std::string reply;
            if (action == 0) {
                be32(reply, 0);
                be32(reply, tx);
                be32(reply, 0x11223344);
                be32(reply, 0x55667788);  // connection_id
            } else if (action == 1 && n >= 98) {
                lastAnnounce = packet;
                if (failAnnounce_) {
                    be32(reply, 3);
                    be32(reply, tx);
                    reply += "torrent not registered";
                } else {
                    be32(reply, 1);
                    be32(reply, tx);
                    be32(reply, 1800);  // interval
                    be32(reply, 1);     // leechers
                    be32(reply, 2);     // seeders
                    reply += std::string("\x0A\x00\x00\x05", 4) + std::string("\x1A\xE1", 2);   // 10.0.0.5:6881
                    reply += std::string("\xC0\xA8\x01\x07", 4) + std::string("\x1F\x90", 2);   // 192.168.1.7:8080
                }
            } else {
                continue;
            }
            ::sendto(socket_, reply.data(), static_cast<int>(reply.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), fromLen);
            ++handled;
        }
    }

#ifdef _WIN32
    SOCKET socket_;
#else
    int socket_;
#endif
    bool failAnnounce_;
    std::uint16_t port_ = 0;
    std::thread thread_;
};

}  // namespace

void testUdpTrackerCodecs() {
    const std::string connect = TrackerClient::buildUdpConnectRequest(0xAABBCCDD);
    CHECK_EQ(connect.size(), std::size_t(16));
    CHECK_EQ(connect.substr(0, 8), std::string("\x00\x00\x04\x17\x27\x10\x19\x80", 8));  // protocol id
    CHECK_EQ(read32(connect, 8), std::uint32_t(0));
    CHECK_EQ(read32(connect, 12), std::uint32_t(0xAABBCCDD));

    std::string connectReply;
    be32(connectReply, 0);
    be32(connectReply, 42);
    be32(connectReply, 1);
    be32(connectReply, 2);
    std::uint64_t connectionId = 0;
    std::string error;
    CHECK(TrackerClient::parseUdpConnectResponse(connectReply, 42, connectionId, error));
    CHECK_EQ(connectionId, std::uint64_t(0x0000000100000002ULL));
    CHECK(!TrackerClient::parseUdpConnectResponse(connectReply, 43, connectionId, error));  // sai transaction
    CHECK(!TrackerClient::parseUdpConnectResponse(connectReply.substr(0, 12), 42, connectionId, error));

    const TrackerRequest request = makeUdpRequest();
    const std::string announce = TrackerClient::buildUdpAnnounceRequest(0x0102030405060708ULL, 7, request, 99);
    CHECK_EQ(announce.size(), std::size_t(98));
    CHECK_EQ(read32(announce, 8), std::uint32_t(1));    // action announce
    CHECK_EQ(announce.substr(16, 20), request.infoHash);
    CHECK_EQ(announce.substr(36, 20), request.peerId);
    CHECK_EQ(read32(announce, 80), std::uint32_t(2));   // event=started
    CHECK_EQ(read32(announce, 92), std::uint32_t(50));  // numwant mặc định
    CHECK_EQ(static_cast<std::uint8_t>(announce[96]), std::uint8_t(0x1A));  // port 6881 big-endian
    CHECK_EQ(static_cast<std::uint8_t>(announce[97]), std::uint8_t(0xE1));

    TrackerRequest badRequest = request;
    badRequest.infoHash = "short";
    CHECK_THROWS(TrackerClient::buildUdpAnnounceRequest(1, 1, badRequest, 1));

    std::string errReply;
    be32(errReply, 3);
    be32(errReply, 7);
    errReply += "nope";
    const TrackerResponse failed = TrackerClient::parseUdpAnnounceResponse(errReply, 7);
    CHECK(!failed.success);
    CHECK_EQ(failed.failureReason, std::string("nope"));

    // Gói peer bị cắt giữa chừng: chỉ giữ các peer đủ 6 byte, không crash.
    std::string truncated;
    be32(truncated, 1);
    be32(truncated, 7);
    be32(truncated, 900);
    be32(truncated, 0);
    be32(truncated, 0);
    truncated += std::string("\x01\x02\x03\x04\x00\x50", 6) + std::string("\x05\x06", 2);
    const TrackerResponse partial = TrackerClient::parseUdpAnnounceResponse(truncated, 7);
    CHECK(partial.success);
    CHECK_EQ(partial.interval, 900);
    CHECK_EQ(partial.peers.size(), std::size_t(1));
    CHECK_EQ(partial.peers[0].ip, std::string("1.2.3.4"));
    CHECK_EQ(partial.peers[0].port, std::uint16_t(80));
}

void testUdpTrackerAnnounceAgainstFakeTracker() {
    FakeUdpTracker tracker(false);
    const std::string url = "udp://127.0.0.1:" + std::to_string(tracker.port()) + "/announce";
    const TrackerResponse response = TrackerClient::announceTracker(url, makeUdpRequest());
    CHECK(response.success);
    CHECK_EQ(response.interval, 1800);
    CHECK_EQ(response.peers.size(), std::size_t(2));
    CHECK_EQ(response.peers[0].ip, std::string("10.0.0.5"));
    CHECK_EQ(response.peers[0].port, std::uint16_t(6881));
    CHECK_EQ(response.peers[1].ip, std::string("192.168.1.7"));
    CHECK_EQ(response.peers[1].port, std::uint16_t(8080));
}

void testUdpTrackerReportsTrackerError() {
    FakeUdpTracker tracker(true);
    const std::string url = "udp://127.0.0.1:" + std::to_string(tracker.port());
    const TrackerResponse response = TrackerClient::announceUdp(url, makeUdpRequest());
    CHECK(!response.success);
    CHECK_EQ(response.failureReason, std::string("torrent not registered"));
}

// ---- HTTPS tracker: máy chủ TLS thật trên loopback, chứng chỉ tự ký (tests/test_https_certs.h) ----
class FakeHttpsTracker {
public:
    FakeHttpsTracker(const char* cert, const char* key) {
        httplib::SSLServer::PemMemory pem{cert, std::strlen(cert), key, std::strlen(key), nullptr, 0, nullptr};
        server_ = std::make_unique<httplib::SSLServer>(pem);
        server_->Get("/announce", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(TrackerProtocol::encodeSuccessResponse(1800, {{"10.9.8.7", 7000}}), "text/plain");
        });
        port_ = server_->bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this]() { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }
    ~FakeHttpsTracker() {
        server_->stop();
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }

private:
    std::unique_ptr<httplib::SSLServer> server_;
    std::thread thread_;
    int port_ = 0;
};

void setEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}

std::string writeTempPem(const std::string& name, const char* pem) {
    const std::string path = name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pem;
    return path;
}

void testHttpsTrackerAnnounceVerifiesCertificates() {
    const std::string goodCa = writeTempPem("test_https_good_ca.pem", test_certs::kGoodCert);
    const std::string wrongCa = writeTempPem("test_https_wrong_ca.pem", test_certs::kWrongCert);
    const TrackerRequest request = makeUdpRequest();

    {
        // CA đúng + tên máy chủ khớp (SAN 127.0.0.1): announce thành công qua TLS.
        FakeHttpsTracker tracker(test_certs::kGoodCert, test_certs::kGoodKey);
        const std::string url = "https://127.0.0.1:" + std::to_string(tracker.port()) + "/announce";
        setEnvVar("P2P_TRACKER_CA_FILE", goodCa);
        const TrackerResponse response = TrackerClient::announceTracker(url, request);
        CHECK(response.success);
        CHECK_EQ(response.peers.size(), std::size_t(1));
        if (!response.peers.empty()) CHECK_EQ(response.peers[0].ip, std::string("10.9.8.7"));

        // Không tin CA này (không có CA tuỳ chọn, kho hệ thống không có chứng chỉ tự ký): phải từ chối, không "thôi kệ".
        setEnvVar("P2P_TRACKER_CA_FILE", "");
        CHECK_THROWS(TrackerClient::announceTracker(url, request));
        // Tracker HTTPS nhận request HTTP thường (sai giao thức) cũng không trả dữ liệu hợp lệ.
        const std::string plainUrl = "http://127.0.0.1:" + std::to_string(tracker.port()) + "/announce";
        CHECK_THROWS(TrackerClient::announceTracker(plainUrl, request));
    }
    {
        // CA được tin nhưng chứng chỉ cấp cho tên khác (SAN wrong.example): sai tên máy chủ bị từ chối.
        FakeHttpsTracker tracker(test_certs::kWrongCert, test_certs::kWrongKey);
        const std::string url = "https://127.0.0.1:" + std::to_string(tracker.port()) + "/announce";
        setEnvVar("P2P_TRACKER_CA_FILE", wrongCa);
        CHECK_THROWS(TrackerClient::announceTracker(url, request));
        // Chính chứng chỉ đó nhưng CA khác tin: cũng từ chối (chứng chỉ không khớp CA).
        setEnvVar("P2P_TRACKER_CA_FILE", goodCa);
        CHECK_THROWS(TrackerClient::announceTracker(url, request));
    }
    setEnvVar("P2P_TRACKER_CA_FILE", "");
    std::remove(goodCa.c_str());
    std::remove(wrongCa.c_str());
}

void testUdpTrackerRejectsBadUrls() {
    const TrackerRequest request = makeUdpRequest();
    CHECK_THROWS(TrackerClient::announceUdp("udp://no-port-here/announce", request));
    CHECK_THROWS(TrackerClient::announceUdp("udp://host:99999", request));
    CHECK_THROWS(TrackerClient::announceUdp("http://host:80/announce", request));
    CHECK_THROWS(TrackerClient::announceTracker("https://host/announce", request));
}

void testTrackerUrlPolicyAllowsNormalTrackers() {
    for (const char* ok : {"http://tracker.example.com:6969/announce", "http://192.168.1.232:6969/announce",
                           "http://127.0.0.1:6969/announce", "http://10.0.0.5/announce", "http://t.example.org/announce.php",
                           "http://t.example.org:8080/abc/announce?passkey=1", "http://t.example.org/Announce",
                           "udp://tracker.opentrackr.org:1337/announce", "udp://192.168.1.5:6969", "udp://localhost:6969"}) {
        const auto check = TrackerClient::checkTrackerUrl(ok);
        CHECK(check.allowed);
        if (!check.allowed) std::cout << "  unexpectedly rejected: " << ok << " (" << check.reason << ")" << std::endl;
    }
}

void testTrackerUrlPolicyBlocksSsrfTargets() {
    // Đường dẫn không phải announce: không cho gọi tới API của dịch vụ khác (vd. WebUI của chính client).
    for (const char* bad : {"http://127.0.0.1:8080/api/torrents/remove", "http://127.0.0.1:8080/", "http://192.168.1.1/cgi-bin/reboot",
                            "http://127.0.0.1:8080/api/files/open", "http://host/not-announce", "http://host/x/announce/../admin"}) {
        const auto check = TrackerClient::checkTrackerUrl(bad);
        CHECK(!check.allowed);
        if (check.allowed) std::cout << "  unexpectedly allowed: " << bad << std::endl;
    }
    // Cloud metadata, địa chỉ đặc biệt.
    CHECK(!TrackerClient::checkTrackerUrl("http://169.254.169.254/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://169.254.1.1/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://metadata.google.internal/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://0.0.0.0/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://224.0.0.1/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://255.255.255.255/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("udp://169.254.169.254:6969").allowed);
    // Cổng dịch vụ nhạy cảm.
    for (int port : {22, 25, 445, 3306, 3389, 5432, 6379, 9200, 11211}) {
        CHECK(!TrackerClient::checkTrackerUrl("http://192.168.1.5:" + std::to_string(port) + "/announce").allowed);
        CHECK(!TrackerClient::checkTrackerUrl("udp://192.168.1.5:" + std::to_string(port)).allowed);
    }
    // Scheme và URL hỏng.
    CHECK(TrackerClient::checkTrackerUrl("https://tracker.example.com/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("https://169.254.169.254/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("https://192.168.1.5:22/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("https://tracker.example.com/api/torrents/remove").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("ftp://tracker.example.com/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("file:///etc/passwd").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("udp://no-port/announce").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http:///announce").allowed);

    // Tracker hiếm dùng tên đường dẫn khác: mặc định bị chặn, P2P_TRACKER_ANY_PATH=1 mở lại; các luật khác vẫn giữ.
    CHECK(!TrackerClient::checkTrackerUrl("http://tracker.example.com/tracker").allowed);
#ifdef _WIN32
    _putenv("P2P_TRACKER_ANY_PATH=1");
#else
    setenv("P2P_TRACKER_ANY_PATH", "1", 1);
#endif
    CHECK(TrackerClient::checkTrackerUrl("http://tracker.example.com/tracker").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://169.254.169.254/tracker").allowed);
    CHECK(!TrackerClient::checkTrackerUrl("http://192.168.1.5:22/tracker").allowed);
#ifdef _WIN32
    _putenv("P2P_TRACKER_ANY_PATH=");
#else
    unsetenv("P2P_TRACKER_ANY_PATH");
#endif
}

void testAnnounceRefusesRejectedUrlsWithoutTouchingTheNetwork() {
    // Cả hai đường ra mạng đều kiểm tra: URL bị từ chối ném lỗi ngay, không tạo kết nối.
    TrackerRequest request = makeUdpRequest();
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(TrackerClient::announceHttp("http://127.0.0.1:1/api/torrents/remove", request));
    CHECK_THROWS(TrackerClient::announceTracker("http://169.254.169.254/announce", request));
    CHECK_THROWS(TrackerClient::announceUdp("udp://169.254.169.254:6969", request));
    CHECK_THROWS(TrackerClient::announceTracker("http://192.168.1.5:22/announce", request));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
}

void testUdpRendezvousCarriesSessionKey() {
    const std::string infoHash(20, 'h');
    const std::string peerId(20, 'p');
    const std::string key("\x01\x02\x03\x04\x05\x06\x07\x08", 8);

    const auto registration = network::udp_rendezvous::encode_register_request(infoHash, peerId, 6881, key);
    network::udp_rendezvous::Request decoded;
    CHECK(network::udp_rendezvous::decode_request(registration.data(), registration.size(), decoded));
    CHECK_EQ(decoded.key, key);

    const auto punch = network::udp_rendezvous::encode_punch_request(infoHash, peerId, 6881, "198.51.100.20", 6882, 7, key);
    CHECK(network::udp_rendezvous::decode_request(punch.data(), punch.size(), decoded));
    CHECK_EQ(decoded.key, key);
    CHECK_EQ(decoded.target_ip, std::string("198.51.100.20"));
    CHECK_EQ(decoded.transaction_id, std::uint32_t(7));

    // Không truyền khoá = 8 byte 0 ("không có khoá", client cũ).
    const auto legacy = network::udp_rendezvous::encode_register_request(infoHash, peerId, 6881);
    CHECK(network::udp_rendezvous::decode_request(legacy.data(), legacy.size(), decoded));
    CHECK_EQ(decoded.key, std::string(8, '\0'));

    // Khoá sai độ dài bị từ chối khi mã hoá.
    CHECK_THROWS(network::udp_rendezvous::encode_register_request(infoHash, peerId, 6881, "short"));
    CHECK_THROWS(network::udp_rendezvous::encode_punch_request(infoHash, peerId, 6881, "198.51.100.20", 6882, 1, "toolong-key"));
    // Gói theo giao thức cũ (48 byte, version 1) không được chấp nhận nữa.
    std::string oldFormat = registration.substr(0, 48);
    oldFormat[4] = 1;
    CHECK(!network::udp_rendezvous::decode_request(oldFormat.data(), oldFormat.size(), decoded));
}

void testRegistryKeyBlocksPeerIdHijack() {
    PeerRegistry registry(3600, 50);
    const std::string infoHash(20, 'h');
    const std::string victimKey("\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22", 8);
    const std::string attackerKey("\x99\x88\x77\x66\x55\x44\x33\x22", 8);
    const auto now = PeerRegistry::Clock::now();

    PeerRegistration victim{"victimPeerId0000000", "192.168.1.10", 6881, 0, 0, 0, victimKey};
    victim.peerId = std::string(20, 'v');
    CHECK(registry.upsertChecked(infoHash, victim, now) == PeerRegistry::UpsertStatus::Ok);
    // Người cùng IP/NAT, biết peer_id của nạn nhân, nhưng không có khoá:
    PeerRegistration hijackWithOtherKey = victim;
    hijackWithOtherKey.key = attackerKey;
    hijackWithOtherKey.port = 6666;
    CHECK(registry.upsertChecked(infoHash, hijackWithOtherKey, now) == PeerRegistry::UpsertStatus::KeyMismatch);
    PeerRegistration hijackWithoutKey = victim;
    hijackWithoutKey.key.clear();
    hijackWithoutKey.port = 6666;
    CHECK(registry.upsertChecked(infoHash, hijackWithoutKey, now) == PeerRegistry::UpsertStatus::KeyMismatch);
    // Chủ khoá vẫn cập nhật được bình thường.
    PeerRegistration refresh = victim;
    refresh.left = 5;
    CHECK(registry.upsertChecked(infoHash, refresh, now) == PeerRegistry::UpsertStatus::Ok);

    // Gửi "stopped" thay nạn nhân không có khoá/sai khoá: không xoá được.
    registry.remove(infoHash, victim.peerId, attackerKey);
    registry.remove(infoHash, victim.peerId, "");
    CHECK(registry.hasPeer(infoHash, victim.peerId));
    registry.remove(infoHash, victim.peerId, victimKey);
    CHECK(!registry.hasPeer(infoHash, victim.peerId));
}

void testRegistryKeyBlocksUdpEndpointHijack() {
    PeerRegistry registry(3600, 50);
    const std::string infoHash(20, 'h');
    const std::string victimKey("\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22", 8);
    const std::string attackerKey("\x99\x88\x77\x66\x55\x44\x33\x22", 8);
    const auto now = PeerRegistry::Clock::now();

    registry.upsert(infoHash, {std::string(20, 'v'), "203.0.113.10", 6881, 0, 0, 0, victimKey}, now);
    registry.upsert(infoHash, {std::string(20, 'w'), "198.51.100.20", 6882, 0, 0, 0, attackerKey}, now);
    CHECK(registry.updateUdpEndpoint(infoHash, std::string(20, 'v'), "203.0.113.10", 41000, 6881, now, victimKey));

    // Cùng NAT (cùng IP nguồn 203.0.113.10), biết peer_id nhưng sai khoá hoặc không có khoá:
    CHECK(!registry.updateUdpEndpoint(infoHash, std::string(20, 'v'), "203.0.113.10", 55555, 6881, now, attackerKey));
    CHECK(!registry.updateUdpEndpoint(infoHash, std::string(20, 'v'), "203.0.113.10", 55555, 6881, now, std::string(8, '\0')));
    CHECK(!registry.updateUdpEndpoint(infoHash, std::string(20, 'v'), "203.0.113.10", 55555, 6881, now));
    // Endpoint của nạn nhân không bị đổi.
    const auto found = registry.getUdpEndpointByTcpEndpoint(infoHash, std::string(20, 'w'), "198.51.100.20", "203.0.113.10", 6881, now, attackerKey);
    CHECK(found.has_value());
    if (found) {
        CHECK_EQ(found->port, std::uint16_t(41000));  // các lần giả mạo không đổi được endpoint thật của nạn nhân
    }
    CHECK(registry.updateUdpEndpoint(infoHash, std::string(20, 'w'), "198.51.100.20", 42000, 6882, now, attackerKey));
    const auto target = registry.getUdpEndpointByTcpEndpoint(infoHash, std::string(20, 'w'), "198.51.100.20", "203.0.113.10", 6881, now, attackerKey);
    CHECK(target.has_value());
    if (target) {
        CHECK_EQ(target->port, std::uint16_t(41000));  // vẫn là cổng thật của nạn nhân
    }
    // Người yêu cầu cũng phải đúng khoá: đứng tên "w" nhưng khoá sai thì không hỏi được.
    CHECK(!registry.getUdpEndpointByTcpEndpoint(infoHash, std::string(20, 'w'), "198.51.100.20", "203.0.113.10", 6881, now, victimKey).has_value());
}

void testRegistryLegacyClientsWithoutKeyStillWork() {
    PeerRegistry registry(3600, 50);
    const std::string infoHash(20, 'h');
    const auto now = PeerRegistry::Clock::now();
    registry.upsert(infoHash, {std::string(20, 'o'), "10.0.0.1", 6881, 0, 0, 0}, now);  // không khoá
    CHECK(registry.updateUdpEndpoint(infoHash, std::string(20, 'o'), "10.0.0.1", 41000, 6881, now));
    // Client cũ được "nâng cấp": lần announce sau có khoá thì được gắn khoá.
    const std::string key("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    CHECK(registry.upsertChecked(infoHash, {std::string(20, 'o'), "10.0.0.1", 6881, 0, 0, 0, key}, now) == PeerRegistry::UpsertStatus::Ok);
    // Từ đó không còn client không khoá nào chiếm được.
    CHECK(registry.upsertChecked(infoHash, {std::string(20, 'o'), "10.0.0.1", 6881, 0, 0, 0}, now) == PeerRegistry::UpsertStatus::KeyMismatch);
}

void testTrackerServerKeyEndToEnd() {
    TrackerServer tracker(1800, 50);
    TrackerRequest request;
    request.infoHash = std::string(20, 'h');
    request.peerId = std::string(20, 'v');
    request.ip = "203.0.113.10";
    request.port = 6881;
    request.event = "started";
    request.key = "aabbccddeeff1122";

    TrackerRequest other = request;
    other.peerId = std::string(20, 'w');
    other.ip = "203.0.113.11";
    other.key = "0011223344556677";
    CHECK(TrackerProtocol::decodeResponse(tracker.announce(request)).success);
    CHECK(TrackerProtocol::decodeResponse(tracker.announce(other)).success);

    // Người cùng IP dùng peer_id của nạn nhân với khoá khác / không khoá: bị từ chối, nạn nhân không bị ghi đè.
    TrackerRequest hijack = request;
    hijack.key = "0000000000000001";
    hijack.port = 9999;
    auto rejected = TrackerProtocol::decodeResponse(tracker.announce(hijack));
    CHECK(!rejected.success);
    CHECK(rejected.failureReason.find("already in use") != std::string::npos);
    hijack.key.clear();
    CHECK(!TrackerProtocol::decodeResponse(tracker.announce(hijack)).success);
    const auto peers = tracker.getPeers(request.infoHash);
    bool victimStillOnOriginalPort = false;
    for (const auto& peer : peers) {
        victimStillOnOriginalPort = victimStillOnOriginalPort || (peer.ip == "203.0.113.10" && peer.port == 6881);
    }
    CHECK(victimStillOnOriginalPort);

    // "stopped" giả mạo không xoá được; "stopped" đúng khoá thì xoá.
    TrackerRequest fakeStop = hijack;
    fakeStop.event = "stopped";
    tracker.announce(fakeStop);
    CHECK(tracker.hasPeer(request.infoHash, request.peerId));
    TrackerRequest realStop = request;
    realStop.event = "stopped";
    tracker.announce(realStop);
    CHECK(!tracker.hasPeer(request.infoHash, request.peerId));

    // Khoá sai định dạng bị từ chối.
    TrackerRequest badKey = other;
    badKey.key = "not-hex-and-wrong-length";
    CHECK(!TrackerProtocol::decodeResponse(tracker.announce(badKey)).success);
    badKey.key = "zzzzzzzzzzzzzzzz";
    CHECK(!TrackerProtocol::decodeResponse(tracker.announce(badKey)).success);

    // UDP: đăng ký qua tracker cũng đòi đúng khoá (khoá thô 8 byte tương ứng hex 0011223344556677).
    const std::string rawKey("\x00\x11\x22\x33\x44\x55\x66\x77", 8);
    CHECK(tracker.registerUdpEndpoint(other.infoHash, other.peerId, other.ip, 41000, other.port, rawKey));
    CHECK(!tracker.registerUdpEndpoint(other.infoHash, other.peerId, other.ip, 55555, other.port, std::string(8, '\x42')));
}

void testQueryCarriesKeyParameter() {
    TrackerRequest request;
    request.infoHash = std::string(20, 'h');
    request.peerId = std::string(20, 'p');
    request.port = 6881;
    CHECK(TrackerProtocol::encodeRequestQuery(request).find("&key=") == std::string::npos);  // client cũ: không có tham số
    request.key = "aabbccddeeff1122";
    CHECK(TrackerProtocol::encodeRequestQuery(request).find("&key=aabbccddeeff1122") != std::string::npos);
}

int main() {
    RUN(testEndpointRateLimiterIsBoundedAndFailsClosed);
    RUN(testEndpointRateLimiterSurvivesFloodOfDistinctSources);
    RUN(testPeerRegistryOneIpCannotEvictOtherIps);
    RUN(testPeerRegistryOneIpCannotFillSwarmTable);
    RUN(testPeerRegistryLanWithSeveralClientsOnOneIpStillWorks);
    RUN(testPeerRegistryRejectsNewSwarmWhenFull);
    RUN(testPeerRegistryRendezvousUsesFreshObservedUdpEndpoints);
    RUN(testUdpRendezvousRegisterAndPunchRequestCodec);
    RUN(testUdpRendezvousResultsAreBoundedAndRoundTrip);
    RUN(testTrackerServerRejectsIpv6Peer);
    RUN(testEncodeRequestQueryPercentEncodesBinaryBytes);
    RUN(testCompactPeersRoundTrip);
    RUN(testDecodeCompactPeersRejectsBadLength);
    RUN(testSuccessResponseRoundTrip);
    RUN(testFailureResponseRoundTrip);
    RUN(testTrackerClientMakePeerIdHasPrefixAndLength);
    RUN(testTrackerClientExtractsUdpRendezvousEndpoint);
    RUN(testTrackerClientRetriesTransientHttpFailures);
    RUN(testPeerRegistryUpsertAndGetPeersExcludesSelf);
    RUN(testPeerRegistryExpiresOldPeers);
    RUN(testPeerRegistryEvictsOldestWhenFull);
    RUN(testTrackerServerAnnounceReturnsOtherPeersOnly);
    RUN(testTrackerServerStoppedEventRemovesPeer);
    RUN(testTrackerServerRejectsMalformedInfoHashOrPeerId);
    RUN(testUdpTrackerCodecs);
    RUN(testUdpTrackerAnnounceAgainstFakeTracker);
    RUN(testUdpTrackerReportsTrackerError);
    RUN(testUdpTrackerRejectsBadUrls);
    RUN(testUdpRendezvousCarriesSessionKey);
    RUN(testRegistryKeyBlocksPeerIdHijack);
    RUN(testRegistryKeyBlocksUdpEndpointHijack);
    RUN(testRegistryLegacyClientsWithoutKeyStillWork);
    RUN(testTrackerServerKeyEndToEnd);
    RUN(testQueryCarriesKeyParameter);
    RUN(testHttpsTrackerAnnounceVerifiesCertificates);
    RUN(testTrackerUrlPolicyAllowsNormalTrackers);
    RUN(testTrackerUrlPolicyBlocksSsrfTargets);
    RUN(testAnnounceRefusesRejectedUrlsWithoutTouchingTheNetwork);
    return TEST_REPORT();
}
