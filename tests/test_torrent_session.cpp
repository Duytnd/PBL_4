// Có assert() trong luồng seeder giả: bật lại cả khi build Release (-DNDEBUG).
#undef NDEBUG

#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/torrent_builder.h"
#include "network/allowed_fast.h"
#include "network/extension.h"
#include "network/magnet_resolver.h"
#include "network/network_monitor.h"
#include "network/peer_connection.h"
#include "network/peer_protocol.h"
#include "network/torrent_session.h"
#include "webui/state_bridge.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace p2p;
using namespace pbl4::network;

namespace {

TorrentFileMetadata makeMockMetadata(
    const std::string& name,
    const std::vector<std::string>& pieceDataList,
    std::size_t pieceLength) {
    TorrentFileMetadata meta;
    meta.name = name;
    meta.pieceLength = static_cast<std::int64_t>(pieceLength);
    meta.announce = "http://127.0.0.1:6969/announce";

    std::string full_content;
    for (const auto& p : pieceDataList) {
        meta.pieces.push_back(sha1Digest(p));
        full_content += p;
    }
    meta.length = full_content.size();

    // Mock infoRaw
    meta.infoRaw = "d6:lengthi" + std::to_string(meta.length) +
                   "e4:name" + std::to_string(meta.name.size()) + ":" + meta.name +
                   "12:piece lengthi" + std::to_string(meta.pieceLength) + "ee";
    return meta;
}

}  // namespace

void testSessionInitializationAndStats() {
    std::cout << "[TEST] TorrentSession initialization and stats...\n";
    const std::vector<std::string> pieces = {"PieceZeroData!", "PieceOneData!!"};
    auto meta = makeMockMetadata("sample_file.bin", pieces, 14);

    TorrentSessionConfig config;
    config.download_dir = "./test_downloads";
    config.listen_port = 6889;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    auto stats = session.stats();

    CHECK_EQ(stats.total_pieces, std::size_t(2));
    CHECK_EQ(stats.completed_pieces, std::size_t(0));
    CHECK_EQ(stats.total_bytes, std::uint64_t(28));
    CHECK_EQ(stats.left_bytes, std::uint64_t(28));
    CHECK(!session.is_complete());
    CHECK(!session.is_running());
    CHECK_EQ(session.metadata().name, std::string("sample_file.bin"));
    CHECK(!session.info_hash_hex().empty());
}

void testKademliaNodeFiltersPublicPeersOnPrivateNetworks() {
    std::cout << "[TEST] Kademlia peer filtering keeps private LAN peers while rejecting public leaks on private WiFi...\n";

    CHECK(!KademliaNode::should_connect_peer("192.168.1.50", "117.3.54.230", true));
    CHECK(KademliaNode::should_connect_peer("192.168.1.50", "192.168.1.25", false));
    CHECK(!KademliaNode::should_connect_peer("8.8.8.8", "117.3.54.230", false));
    CHECK(KademliaNode::should_connect_peer("10.0.0.5", "127.0.0.1", false));

    // Tailscale / CGNAT (100.64.0.0/10) là mạng nội bộ, không bị coi là public.
    CHECK(KademliaNode::is_private_ipv4("100.101.102.103"));
    CHECK(KademliaNode::is_private_ipv4("100.64.0.1"));
    CHECK(KademliaNode::is_private_ipv4("100.127.255.254"));
    CHECK(!KademliaNode::is_private_ipv4("100.63.255.255"));
    CHECK(!KademliaNode::is_private_ipv4("100.128.0.1"));
    CHECK(KademliaNode::should_connect_peer("100.71.46.96", "100.88.1.2", false));
    CHECK(KademliaNode::should_connect_peer("192.168.1.50", "100.88.1.2", true));

    // Mạng cấp IP public / chủ động bật: bộ lọc LAN tắt, vẫn chặn địa chỉ rác.
    CHECK(KademliaNode::should_connect_peer("8.8.8.8", "117.3.54.230", false, true));
    CHECK(KademliaNode::should_connect_peer("192.168.1.50", "117.3.54.230", true, true));
    CHECK(!KademliaNode::should_connect_peer("8.8.8.8", "169.254.1.1", false, true));
    CHECK(!KademliaNode::should_connect_peer("8.8.8.8", "", false, true));
}

void testMagnetResolverFetchesAndVerifiesMetadata() {
    std::cout << "[TEST] Magnet resolver retrieves BEP 9 metadata and verifies BTIH...\n";
    const std::filesystem::path temp_dir = "./test_magnet_resolver";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::string file_data = "magnet metadata integration sample";
    const auto source_path = temp_dir / "seed.txt";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << file_data;
    }
    const auto torrent_bytes = TorrentBuilder::buildTorrent(source_path, "", 16384);
    const auto expected_metadata = TorrentFile::parse(torrent_bytes);
    const std::string raw_hash = sha1Digest(expected_metadata.infoRaw);

    asio::io_context server_ioc;
    asio::ip::tcp::acceptor acceptor(
        server_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    std::exception_ptr server_error;

    std::thread server([&]() {
        try {
            PeerConnection peer(acceptor.accept(), std::chrono::seconds(5));
            const auto incoming = peer.receive_handshake();
            PeerHandshake reply;
            std::memcpy(reply.info_hash.data(), raw_hash.data(), raw_hash.size());
            reply.peer_id.fill(0x66);
            reply.set_supports_extension(true);
            peer.send_handshake(reply);

            const auto local_extension_handshake = peer.receive_message();
            if (local_extension_handshake.id != static_cast<Byte>(PeerMessageId::Extended) ||
                local_extension_handshake.payload.empty() ||
                local_extension_handshake.payload[0] != extension::kExtendedHandshakeId ||
                incoming.info_hash != reply.info_hash) {
                throw std::runtime_error("resolver sent an invalid handshake");
            }

            BencodeDict mappings;
            mappings.emplace(extension::kUtMetadataName, BencodeValue(std::int64_t(9)));
            BencodeDict extended;
            extended.emplace("m", BencodeValue(std::move(mappings)));
            extended.emplace("metadata_size",
                BencodeValue(static_cast<std::int64_t>(expected_metadata.infoRaw.size())));
            const std::string extended_bytes = encodeBencode(extended);
            peer.send_message(peer_protocol::make_extended(
                extension::kExtendedHandshakeId,
                Bytes(extended_bytes.begin(), extended_bytes.end())));

            const auto request = peer.receive_message();
            const auto parsed_request = extension::parse_metadata_message(request, 9);
            if (parsed_request.type != extension::MetadataMessage::Type::Request ||
                parsed_request.piece != 0) {
                throw std::runtime_error("resolver sent an invalid metadata request");
            }
            const auto* begin = reinterpret_cast<const Byte*>(expected_metadata.infoRaw.data());
            const Bytes metadata_block(begin, begin + expected_metadata.infoRaw.size());
            peer.send_message(extension::make_metadata_data(
                extension::kUtMetadataId, 0,
                static_cast<std::uint32_t>(expected_metadata.infoRaw.size()), metadata_block));
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    network::MagnetLink magnet;
    magnet.info_hash = raw_hash;
    magnet.info_hash_hex = sha1Hex(expected_metadata.infoRaw);
    const auto resolved = network::fetch_magnet_metadata_from_peers(
        magnet, {{"127.0.0.1", port}}, std::string(20, 'R'));
    server.join();
    if (server_error) {
        std::rethrow_exception(server_error);
    }

    CHECK_EQ(resolved.infoRaw, expected_metadata.infoRaw);
    CHECK_EQ(resolved.name, expected_metadata.name);
    CHECK(resolved.pieces == expected_metadata.pieces);
    std::filesystem::remove_all(temp_dir, ec);
}

void testMagnetResolverRetriesUntilTrackerHasPeers() {
    std::cout << "[TEST] Magnet resolver retries tracker until a peer shows up..." << std::endl;
    const std::filesystem::path temp_dir = "./test_magnet_retry";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    const auto source_path = temp_dir / "retry.txt";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "retry metadata sample";
    }
    const auto torrent_bytes = TorrentBuilder::buildTorrent(source_path, "", 16384);
    const auto expected_metadata = TorrentFile::parse(torrent_bytes);
    const std::string raw_hash = sha1Digest(expected_metadata.infoRaw);
    const std::string hash_hex = sha1Hex(expected_metadata.infoRaw);

    // Peer giả phục vụ metadata (BEP 9).
    asio::io_context peer_ioc;
    asio::ip::tcp::acceptor peer_acceptor(
        peer_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const std::uint16_t peer_port = peer_acceptor.local_endpoint().port();
    std::thread peer_thread([&]() {
        try {
            PeerConnection peer(peer_acceptor.accept(), std::chrono::seconds(5));
            peer.receive_handshake();
            PeerHandshake reply;
            std::memcpy(reply.info_hash.data(), raw_hash.data(), raw_hash.size());
            reply.peer_id.fill(0x67);
            reply.set_supports_extension(true);
            peer.send_handshake(reply);
            peer.receive_message();
            BencodeDict mappings;
            mappings.emplace(extension::kUtMetadataName, BencodeValue(std::int64_t(9)));
            BencodeDict extended;
            extended.emplace("m", BencodeValue(std::move(mappings)));
            extended.emplace("metadata_size",
                BencodeValue(static_cast<std::int64_t>(expected_metadata.infoRaw.size())));
            const std::string extended_bytes = encodeBencode(extended);
            peer.send_message(peer_protocol::make_extended(
                extension::kExtendedHandshakeId, Bytes(extended_bytes.begin(), extended_bytes.end())));
            peer.receive_message();  // metadata request
            const auto* begin = reinterpret_cast<const Byte*>(expected_metadata.infoRaw.data());
            peer.send_message(extension::make_metadata_data(
                extension::kUtMetadataId, 0, static_cast<std::uint32_t>(expected_metadata.infoRaw.size()),
                Bytes(begin, begin + expected_metadata.infoRaw.size())));
        } catch (...) {
        }
    });

    // Tracker HTTP giả: 2 lần đầu không có peer, từ lần 3 mới trả peer.
    asio::io_context tracker_ioc;
    asio::ip::tcp::acceptor tracker_acceptor(
        tracker_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const std::uint16_t tracker_port = tracker_acceptor.local_endpoint().port();
    std::atomic<int> announce_count{0};
    std::atomic<bool> stop_tracker{false};
    std::thread tracker_thread([&]() {
        while (!stop_tracker) {
            try {
                asio::ip::tcp::socket socket(tracker_ioc);
                asio::error_code accept_ec;
                tracker_acceptor.non_blocking(true);
                tracker_acceptor.accept(socket, accept_ec);
                if (accept_ec) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                socket.non_blocking(false);
                asio::streambuf request;
                asio::read_until(socket, request, "\r\n\r\n");
                const int call = ++announce_count;
                std::vector<TrackerPeer> peers;
                if (call >= 3) {
                    peers.push_back({"127.0.0.1", peer_port});
                }
                const std::string body = TrackerProtocol::encodeSuccessResponse(1, peers);
                const std::string http = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
                asio::write(socket, asio::buffer(http));
            } catch (...) {
            }
        }
    });

    network::MagnetResolveOptions options;
    options.listen_port = 7100;
    options.enable_dht = false;
    options.retry_interval = std::chrono::milliseconds(150);
    options.retry_timeout = std::chrono::seconds(15);
    const std::string uri = "magnet:?xt=urn:btih:" + hash_hex + "&tr=http%3A%2F%2F127.0.0.1%3A" +
                            std::to_string(tracker_port) + "%2Fannounce";

    TorrentFileMetadata resolved;
    bool resolved_ok = false;
    try {
        resolved = network::resolve_magnet_uri(uri, options);
        resolved_ok = true;
    } catch (const std::exception& e) {
        std::cout << "  resolve failed: " << e.what() << std::endl;
    }
    CHECK(resolved_ok);
    if (resolved_ok) {
        CHECK_EQ(resolved.infoRaw, expected_metadata.infoRaw);
        CHECK_EQ(resolved.name, expected_metadata.name);
    }
    CHECK(announce_count.load() >= 3);  // phải hỏi lại tracker ít nhất 2 lần sau lần rỗng đầu tiên

    stop_tracker = true;
    tracker_thread.join();
    peer_acceptor.close();
    if (peer_thread.joinable()) {
        peer_thread.join();
    }
    std::filesystem::remove_all(temp_dir, ec);
}

void testMagnetResolverFailsFastWithoutAnySource() {
    std::cout << "[TEST] Magnet resolver fails fast when there is no tracker or DHT to retry..." << std::endl;
    network::MagnetResolveOptions options;
    options.listen_port = 7101;
    options.enable_dht = false;
    options.retry_timeout = std::chrono::seconds(20);  // nếu lỡ thử lại thì test sẽ chạy rất lâu
    const std::string uri = "magnet:?xt=urn:btih:" + std::string(40, 'a');

    const auto started = std::chrono::steady_clock::now();
    CHECK_THROWS(network::resolve_magnet_uri(uri, options));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(3));

    // URI sai là lỗi người dùng: báo ngay, không thử lại.
    const auto started_bad = std::chrono::steady_clock::now();
    CHECK_THROWS(network::resolve_magnet_uri("not-a-magnet", options));
    CHECK(std::chrono::steady_clock::now() - started_bad < std::chrono::seconds(3));
}

void testMagnetFetchStopsWhenAttemptBudgetIsExhausted() {
    std::cout << "[TEST] Magnet metadata fetch gives up when the attempt budget is spent..." << std::endl;
    network::MagnetLink magnet;
    magnet.info_hash = std::string(20, 'm');
    // 8 peer không với tới được (TEST-NET-1 bị chặn): nếu thử hết, mỗi peer chờ tới 5 giây kết nối quá hạn.
    std::vector<TrackerPeer> unreachable;
    for (std::uint16_t i = 0; i < 8; ++i) {
        unreachable.push_back({"192.0.2." + std::to_string(i + 1), static_cast<std::uint16_t>(6881 + i)});
    }
    const auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        network::fetch_magnet_metadata_from_peers(magnet, unreachable, std::string(20, 'p'), "", std::chrono::milliseconds(700));
    } catch (const std::exception&) {
        threw = true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(threw);
    CHECK(elapsed < std::chrono::seconds(14));  // không có hạn: 8 x 5 giây = 40 giây
}

void testTorrentSessionServesBep9Metadata() {
    std::cout << "[TEST] TorrentSession serves BEP 9 metadata to an extension peer...\n";
    const std::filesystem::path temp_dir = "./test_metadata_serve";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "metadata_seed.txt";
    const std::string source_data = "session metadata serving sample";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << source_data;
    }
    const auto torrent_bytes = TorrentBuilder::buildTorrent(source_path, "", 16384);
    const auto metadata = TorrentFile::parse(torrent_bytes);

    asio::io_context port_ioc;
    asio::ip::tcp::acceptor port_probe(
        port_ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto listen_port = port_probe.local_endpoint().port();
    port_probe.close();

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = listen_port;
    config.enable_tracker = false;
    config.enable_lsd = false;
    config.enable_dht = false;
    TorrentSession session(metadata, config);
    CHECK(session.start());

    PeerConnection client = PeerConnection::connect_to(
        "127.0.0.1", listen_port, std::chrono::seconds(5));
    PeerHandshake local_handshake;
    const std::string raw_hash = sha1Digest(metadata.infoRaw);
    std::memcpy(local_handshake.info_hash.data(), raw_hash.data(), raw_hash.size());
    local_handshake.peer_id.fill('M');
    local_handshake.set_supports_extension(true);
    client.send_handshake(local_handshake);
    const auto remote_handshake = client.receive_handshake();
    CHECK(remote_handshake.supports_extension());

    client.send_bitfield(std::vector<bool>{false});
    client.send_message(extension::make_extended_handshake());

    const auto bitfield = client.receive_message();
    CHECK_EQ(bitfield.id, static_cast<Byte>(PeerMessageId::Bitfield));
    const auto extended_handshake = client.receive_message();
    const auto mappings = extension::parse_extended_handshake(extended_handshake);
    CHECK_EQ(extension::parse_metadata_size(extended_handshake).value(),
        static_cast<std::uint32_t>(metadata.infoRaw.size()));

    client.send_message(extension::make_metadata_request(
        mappings.at(extension::kUtMetadataName), 0));
    PeerMessage metadata_response;
    do {
        metadata_response = client.receive_message();
    } while (metadata_response.id != static_cast<Byte>(PeerMessageId::Extended) ||
        metadata_response.payload.empty() ||
        metadata_response.payload[0] != extension::kUtMetadataId);
    const auto parsed = extension::parse_metadata_message(
        metadata_response, extension::kUtMetadataId);
    CHECK(parsed.type == extension::MetadataMessage::Type::Data);
    CHECK_EQ(parsed.total_size, static_cast<std::uint32_t>(metadata.infoRaw.size()));
    CHECK_EQ(std::string(parsed.data.begin(), parsed.data.end()), metadata.infoRaw);

    client.close();
    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testTorrentSessionConnectsToPexPeer() {
    std::cout << "[TEST] TorrentSession connects to a peer learned through PEX...\n";
    const std::filesystem::path temp_dir = "./test_pex_discovery";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "pex_seed.txt";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "pex discovery integration sample";
    }
    const auto torrent_bytes = TorrentBuilder::buildTorrent(source_path, "", 16384);
    const auto metadata = TorrentFile::parse(torrent_bytes);
    const std::string raw_hash = sha1Digest(metadata.infoRaw);

    asio::io_context port_ioc;
    asio::ip::tcp::acceptor session_port_probe(
        port_ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto session_port = session_port_probe.local_endpoint().port();
    session_port_probe.close();
    asio::ip::tcp::acceptor pex_listener(
        port_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto pex_port = pex_listener.local_endpoint().port();

    std::promise<void> discovered_connection;
    auto discovered_future = discovered_connection.get_future();
    std::thread discovered_peer([&]() {
        try {
            PeerConnection peer(pex_listener.accept(), std::chrono::seconds(5));
            const auto incoming = peer.receive_handshake();
            if (incoming.info_hash != InfoHash([&]() {
                    InfoHash hash{};
                    std::memcpy(hash.data(), raw_hash.data(), raw_hash.size());
                    return hash;
                }())) {
                throw std::runtime_error("PEX-discovered connection used the wrong info_hash");
            }
            PeerHandshake reply;
            reply.info_hash = incoming.info_hash;
            reply.peer_id.fill('P');
            peer.send_handshake(reply);
            discovered_connection.set_value();
        } catch (...) {
            try {
                discovered_connection.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    });

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = session_port;
    config.enable_tracker = false;
    config.enable_lsd = false;
    config.enable_dht = false;
    TorrentSession session(metadata, config);
    CHECK(session.start());

    PeerConnection client = PeerConnection::connect_to(
        "127.0.0.1", session_port, std::chrono::seconds(5));
    PeerHandshake local_handshake;
    std::memcpy(local_handshake.info_hash.data(), raw_hash.data(), raw_hash.size());
    local_handshake.peer_id.fill('X');
    local_handshake.set_supports_extension(true);
    client.send_handshake(local_handshake);
    CHECK(client.receive_handshake().supports_extension());
    client.send_bitfield(std::vector<bool>{false});
    client.send_message(extension::make_extended_handshake());

    const auto remote_bitfield = client.receive_message();
    CHECK_EQ(remote_bitfield.id, static_cast<Byte>(PeerMessageId::Bitfield));
    const auto remote_extended = client.receive_message();
    const auto remote_extensions = extension::parse_extended_handshake(remote_extended);
    CHECK(remote_extensions.find(extension::kUtPexName) != remote_extensions.end());

    client.send_message(extension::make_pex_message(
        extension::kUtPexId, {{"127.0.0.1", pex_port}}));
    const bool connected = discovered_future.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    CHECK(connected);
    if (connected) {
        discovered_future.get();
    } else {
        asio::error_code ignored;
        pex_listener.close(ignored);
    }

    client.close();
    session.stop();
    if (discovered_peer.joinable()) discovered_peer.join();
    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionExistingFileSeeding() {
    std::cout << "[TEST] TorrentSession existing file detection (Seeder mode)...\n";
    const std::filesystem::path temp_dir = "./test_existing_seeder";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::vector<std::string> pieces = {"AlphaPiece123456", "BetaPiece1234567"};
    auto meta = makeMockMetadata("existing_file.txt", pieces, 16);

    // Ghi sẵn file hoàn chỉnh vào thư mục tải
    const std::filesystem::path file_path = temp_dir / meta.name;
    {
        std::ofstream out(file_path, std::ios::binary);
        out << pieces[0] << pieces[1];
    }

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6890;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    auto stats = session.stats();

    CHECK_EQ(stats.completed_pieces, std::size_t(2));
    CHECK_EQ(stats.left_bytes, std::uint64_t(0));
    CHECK_EQ(stats.progress_percent, 100.0);
    CHECK(session.is_complete());
    CHECK_EQ(stats.state, SessionState::Seeding);

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionPartialExistingFileRemainsDownloading() {
    std::cout << "[TEST] TorrentSession leaves partial existing file in Downloading state...\n";
    const std::filesystem::path temp_dir = "./test_partial_download";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::vector<std::string> pieces = {"AlphaPiece123456", "BetaPiece1234567"};
    auto meta = makeMockMetadata("partial_file.bin", pieces, 16);

    const std::filesystem::path file_path = temp_dir / meta.name;
    {
        std::ofstream out(file_path, std::ios::binary);
        out << pieces[0];
    }

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6893;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    auto stats = session.stats();

    CHECK_EQ(stats.completed_pieces, std::size_t(1));
    CHECK_EQ(stats.state, SessionState::Downloading);
    CHECK(!session.is_complete());
    CHECK(stats.progress_percent > 0.0);

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionFailsWhenListenPortIsAlreadyBusy() {
    std::cout << "[TEST] TorrentSession startup fails when listen port is already occupied...\n";

    asio::io_context ioc;
    asio::ip::tcp::acceptor busy_acceptor(
        ioc,
        asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const std::uint16_t busy_port = busy_acceptor.local_endpoint().port();

    const std::vector<std::string> pieces = {"Alpha", "Beta"};
    auto meta = makeMockMetadata("busy_port_file.bin", pieces, 5);

    TorrentSessionConfig config;
    config.download_dir = "./test_busy_port";
    config.listen_port = busy_port;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    CHECK_EQ(session.start(), false);
    CHECK_EQ(session.state(), SessionState::Error);

    std::error_code ec;
    std::filesystem::remove_all("./test_busy_port", ec);
}

void testStateBridgeKeepsOriginalMagnetForDuplicateTorrent() {
    std::cout << "[TEST] StateBridge keeps original session when same torrent hash is added again...\n";

    const std::vector<std::string> pieces = {"SamePieceA", "SamePieceB"};
    auto meta = makeMockMetadata("duplicate.bin", pieces, 10);

    TorrentSessionConfig config;
    config.download_dir = "./test_duplicate_bridge";
    config.listen_port = 6902;
    config.enable_lsd = false;

    auto first = std::make_shared<TorrentSession>(meta, config);
    auto second = std::make_shared<TorrentSession>(meta, config);

    StateBridge::instance().addSession(first, "magnet:?xt=urn:btih:first");
    StateBridge::instance().addSession(second, "magnet:?xt=urn:btih:second");

    const auto torrents = StateBridge::instance().getAllTorrents();
    CHECK_EQ(torrents.size(), std::size_t(1));
    CHECK_EQ(torrents[0].magnet_uri, std::string("magnet:?xt=urn:btih:first"));

    StateBridge::instance().removeSession(first->info_hash_hex());
    std::error_code ec;
    std::filesystem::remove_all("./test_duplicate_bridge", ec);
}

void testSessionLoopbackPeerDownload() {
    std::cout << "[TEST] TorrentSession loopback peer download and file assembly...\n";

    const std::filesystem::path temp_dir = "./test_loopback_dl";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::string piece0 = "0123456789ABCDEF";  // 16 bytes
    const std::string piece1 = "GHIJKLMNOPQRSTUV";  // 16 bytes
    auto meta = makeMockMetadata("downloaded_target.bin", {piece0, piece1}, 16);
    meta.announce = "";

    // Tạo server giả lập đóng vai trò Seeder ở cổng 6891
    asio::io_context seeder_ioc;
    asio::ip::tcp::acceptor acceptor(
        seeder_ioc,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 6891));

    InfoHash expected_hash{};
    std::string raw_hash = p2p::sha1Digest(meta.infoRaw);
    std::memcpy(expected_hash.data(), raw_hash.data(), 20);

    // Khởi tạo thread chạy seeder giả lập
    std::thread seeder_thread([&]() {
        try {
            std::cout << "  [SEEDER] Waiting for connection on 6891...\n";
            asio::ip::tcp::socket sock = acceptor.accept();
            std::cout << "  [SEEDER] Accepted socket!\n";
            PeerConnection seeder_peer(std::move(sock), std::chrono::seconds(5));

            // 1. Nhận handshake từ TorrentSession
            auto hs = seeder_peer.receive_handshake();
            std::cout << "  [SEEDER] Handshake received!\n";
            assert(hs.info_hash == expected_hash);

            // Gửi handshake phản hồi
            PeerHandshake reply_hs;
            reply_hs.info_hash = expected_hash;
            reply_hs.peer_id.fill(0x55);
            seeder_peer.send_handshake(reply_hs);
            std::cout << "  [SEEDER] Reply handshake sent!\n";

            // 2. Nhận bitfield từ TorrentSession
            auto bitfield_msg = seeder_peer.receive_message();
            std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(bitfield_msg.id) << "\n";
            assert(bitfield_msg.id == static_cast<Byte>(PeerMessageId::Bitfield));

            // Seeder gửi bitfield của mình (có cả 2 mảnh)
            std::vector<bool> all_have = {true, true};
            seeder_peer.send_bitfield(all_have);
            std::cout << "  [SEEDER] Sent bitfield {true, true}!\n";

            // 3. Nhận interested từ TorrentSession (bỏ qua Port nếu có DHT)
            auto interested_msg = seeder_peer.receive_message();
            while (interested_msg.id == static_cast<Byte>(PeerMessageId::Port)) {
                interested_msg = seeder_peer.receive_message();
            }
            std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(interested_msg.id) << "\n";
            assert(interested_msg.id == static_cast<Byte>(PeerMessageId::Interested));

            // Seeder unchoke client
            seeder_peer.send_unchoke();
            std::cout << "  [SEEDER] Sent unchoke!\n";

            // 4. Phục vụ các block request từ TorrentSession
            int served_blocks = 0;
            while (served_blocks < 2) {
                auto msg = seeder_peer.receive_message();
                std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(msg.id) << "\n";
                if (msg.id == static_cast<Byte>(PeerMessageId::Request)) {
                    auto req = peer_protocol::parse_request(msg);
                    std::cout << "  [SEEDER] Request for piece " << req.index << ", begin " << req.begin << ", len " << req.length << "\n";
                    if (req.index == 0) {
                        seeder_peer.send_piece(0, 0, reinterpret_cast<const Byte*>(piece0.data()), piece0.size());
                        served_blocks++;
                    } else if (req.index == 1) {
                        seeder_peer.send_piece(1, 0, reinterpret_cast<const Byte*>(piece1.data()), piece1.size());
                        served_blocks++;
                    }
                }
            }

            // Nhận các thông điệp Have
            while (seeder_peer.is_open()) {
                auto msg = seeder_peer.receive_message();
                if (msg.id == static_cast<Byte>(PeerMessageId::Have)) {
                    std::cout << "  [SEEDER] Client sent have!\n";
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  [SEEDER] Exception: " << e.what() << "\n";
        }
    });

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6892;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    bool piece_completed_called = false;
    bool torrent_finished_called = false;

    session.on_piece_completed([&](std::size_t idx, std::size_t completed, std::size_t total) {
        piece_completed_called = true;
    });

    session.on_torrent_finished([&]() {
        torrent_finished_called = true;
    });

    session.start();
    session.add_peer("127.0.0.1", 6891);

    // Chờ session kết nối và tải hoàn tất (tối đa 5 giây)
    const auto start_time = std::chrono::steady_clock::now();
    while (!session.is_complete() &&
           std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(session.is_complete());
    CHECK(piece_completed_called);
    CHECK(torrent_finished_called);

    // Kiểm tra file được ghi ra đĩa chính xác
    const std::filesystem::path output_file = temp_dir / meta.name;
    CHECK(std::filesystem::exists(output_file));

    {
        std::ifstream in(output_file, std::ios::binary);
        std::string file_content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_EQ(file_content, piece0 + piece1);
    }

    session.stop();
    if (seeder_thread.joinable()) {
        seeder_thread.join();
    }

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionLoopbackDownloadWithUnreachableTracker() {
    std::cout << "[TEST] TorrentSession loopback download with unreachable tracker (no deadlock on completion)...\n";

    const std::filesystem::path temp_dir = "./test_loopback_dl_tracker";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::string piece0 = "0123456789ABCDEF";  // 16 bytes
    const std::string piece1 = "GHIJKLMNOPQRSTUV";  // 16 bytes
    auto meta = makeMockMetadata("downloaded_target.bin", {piece0, piece1}, 16);
    meta.announce = "http://127.0.0.1:1/announce";  // tracker chết nhưng khác rỗng

    // Tạo server giả lập đóng vai trò Seeder ở cổng 6893
    asio::io_context seeder_ioc;
    asio::ip::tcp::acceptor acceptor(
        seeder_ioc,
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 6893));

    InfoHash expected_hash{};
    std::string raw_hash = p2p::sha1Digest(meta.infoRaw);
    std::memcpy(expected_hash.data(), raw_hash.data(), 20);

    // Khởi tạo thread chạy seeder giả lập
    std::thread seeder_thread([&]() {
        try {
            std::cout << "  [SEEDER] Waiting for connection on 6893...\n";
            asio::ip::tcp::socket sock = acceptor.accept();
            std::cout << "  [SEEDER] Accepted socket!\n";
            PeerConnection seeder_peer(std::move(sock), std::chrono::seconds(5));

            // 1. Nhận handshake từ TorrentSession
            auto hs = seeder_peer.receive_handshake();
            std::cout << "  [SEEDER] Handshake received!\n";
            assert(hs.info_hash == expected_hash);

            // Gửi handshake phản hồi
            PeerHandshake reply_hs;
            reply_hs.info_hash = expected_hash;
            reply_hs.peer_id.fill(0x55);
            seeder_peer.send_handshake(reply_hs);
            std::cout << "  [SEEDER] Reply handshake sent!\n";

            // 2. Nhận bitfield từ TorrentSession
            auto bitfield_msg = seeder_peer.receive_message();
            std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(bitfield_msg.id) << "\n";
            assert(bitfield_msg.id == static_cast<Byte>(PeerMessageId::Bitfield));

            // Seeder gửi bitfield của mình (có cả 2 mảnh)
            std::vector<bool> all_have = {true, true};
            seeder_peer.send_bitfield(all_have);
            std::cout << "  [SEEDER] Sent bitfield {true, true}!\n";

            // 3. Nhận interested từ TorrentSession (bỏ qua Port nếu có DHT)
            auto interested_msg = seeder_peer.receive_message();
            while (interested_msg.id == static_cast<Byte>(PeerMessageId::Port)) {
                interested_msg = seeder_peer.receive_message();
            }
            std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(interested_msg.id) << "\n";
            assert(interested_msg.id == static_cast<Byte>(PeerMessageId::Interested));

            // Seeder unchoke client
            seeder_peer.send_unchoke();
            std::cout << "  [SEEDER] Sent unchoke!\n";

            // 4. Phục vụ các block request từ TorrentSession
            int served_blocks = 0;
            while (served_blocks < 2) {
                auto msg = seeder_peer.receive_message();
                std::cout << "  [SEEDER] Received message ID: " << static_cast<int>(msg.id) << "\n";
                if (msg.id == static_cast<Byte>(PeerMessageId::Request)) {
                    auto req = peer_protocol::parse_request(msg);
                    std::cout << "  [SEEDER] Request for piece " << req.index << ", begin " << req.begin << ", len " << req.length << "\n";
                    if (req.index == 0) {
                        seeder_peer.send_piece(0, 0, reinterpret_cast<const Byte*>(piece0.data()), piece0.size());
                        served_blocks++;
                    } else if (req.index == 1) {
                        seeder_peer.send_piece(1, 0, reinterpret_cast<const Byte*>(piece1.data()), piece1.size());
                        served_blocks++;
                    }
                }
            }

            // Nhận các thông điệp Have
            while (seeder_peer.is_open()) {
                auto msg = seeder_peer.receive_message();
                if (msg.id == static_cast<Byte>(PeerMessageId::Have)) {
                    std::cout << "  [SEEDER] Client sent have!\n";
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  [SEEDER] Exception: " << e.what() << "\n";
        }
    });

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6894;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    bool piece_completed_called = false;
    bool torrent_finished_called = false;

    session.on_piece_completed([&](std::size_t idx, std::size_t completed, std::size_t total) {
        piece_completed_called = true;
    });

    session.on_torrent_finished([&]() {
        torrent_finished_called = true;
    });

    session.start();
    session.add_peer("127.0.0.1", 6893);

    // Chờ session kết nối và tải hoàn tất (tối đa 5 giây)
    const auto start_time = std::chrono::steady_clock::now();
    while (!session.is_complete() &&
           std::chrono::steady_clock::now() - start_time < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(session.is_complete());
    // stats() phải trả về được (trước đây finalize khoá lại mutex đang giữ nên treo vĩnh viễn)
    CHECK_EQ(session.stats().state_name, std::string("seeding"));
    // Tải xong thì file tạm .part phải được dọn.
    CHECK(!std::filesystem::exists(temp_dir / (meta.name + ".part")));
    CHECK(piece_completed_called);
    CHECK(torrent_finished_called);

    // Kiểm tra file được ghi ra đĩa chính xác
    const std::filesystem::path output_file = temp_dir / meta.name;
    CHECK(std::filesystem::exists(output_file));

    {
        std::ifstream in(output_file, std::ios::binary);
        std::string file_content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_EQ(file_content, piece0 + piece1);
    }

    session.stop();
    if (seeder_thread.joinable()) {
        seeder_thread.join();
    }

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionResumesFromPartFile() {
    std::cout << "[TEST] TorrentSession resumes partial download from .part file...\n";
    const std::filesystem::path temp_dir = "./test_resume_part";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    const std::string piece0 = "0123456789ABCDEF";
    const std::string piece1 = "GHIJKLMNOPQRSTUV";
    auto meta = makeMockMetadata("resume_target.bin", {piece0, piece1}, 16);
    meta.announce = "";

    // .part: piece 0 đúng, piece 1 là vùng chưa tải (toàn 0).
    {
        std::ofstream part(temp_dir / "resume_target.bin.part", std::ios::binary);
        part << piece0 << std::string(16, '\0');
    }

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6895;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    const auto stats = session.stats();
    CHECK_EQ(stats.completed_pieces, std::size_t(1));
    CHECK_EQ(stats.left_bytes, std::uint64_t(16));
    CHECK(session.bitfield()[0]);
    CHECK(!session.bitfield()[1]);
    CHECK(!session.is_complete());

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionTrackerDisabledStillRunsDht() {
    std::cout << "[TEST] TorrentSession tracker-disabled + DHT-enabled mode...\n";
    const std::vector<std::string> pieces = {"DhtTestPieceData1", "DhtTestPieceData2"};
    auto meta = makeMockMetadata("tracker_disabled_dht.bin", pieces, 17);

    TorrentSessionConfig config;
    config.download_dir = "./test_dht_downloads_no_tracker";
    config.listen_port = 6897;
    config.enable_tracker = false;
    config.enable_lsd = false;
    config.enable_dht = true;
    config.dht_port = 17800;

    TorrentSession session(meta, config);
    CHECK(session.start());
    CHECK(session.is_running());
    CHECK(session.dht() != nullptr);
    CHECK_EQ(session.dht()->port(), std::uint16_t(17800));

    session.stop();
    CHECK(!session.is_running());
}

void testSessionDhtIntegration() {
    std::cout << "[TEST] TorrentSession DHT integration (BEP 5)...\n";
    const std::vector<std::string> pieces = {"DhtTestPieceData1", "DhtTestPieceData2"};
    auto meta = makeMockMetadata("dht_test_file.bin", pieces, 17);

    TorrentSessionConfig config;
    config.download_dir = "./test_dht_downloads";
    config.listen_port = 6896;
    config.enable_lsd = false;
    config.enable_dht = true;
    config.dht_port = 17799;

    TorrentSession session(meta, config);
    CHECK(session.start());
    CHECK(session.is_running());
    CHECK(session.dht() != nullptr);
    CHECK_EQ(session.dht()->port(), std::uint16_t(17799));

    auto stats = session.stats();
    CHECK_EQ(stats.dht_nodes_count, std::size_t(0));

    session.stop();
    CHECK(!session.is_running());
}

void testSessionStartDoesNotBlockOnDhtBootstrap() {
    std::cout << "[TEST] TorrentSession startup ignores slow DHT bootstrap and returns promptly...\n";
    const std::vector<std::string> pieces = {"BootstrapDelayPieceA", "BootstrapDelayPieceB"};
    auto meta = makeMockMetadata("bootstrap_delay.bin", pieces, 18);

    TorrentSessionConfig config;
    config.download_dir = "./test_dht_bootstrap_delay";
    config.listen_port = 6903;
    config.enable_lsd = false;
    config.enable_dht = true;
    config.dht_port = 17801;
    config.dht_bootstrap_host = "203.0.113.1";
    config.dht_bootstrap_port = 6881;

    TorrentSession session(meta, config);
    const auto start = std::chrono::steady_clock::now();
    CHECK(session.start());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(elapsed < std::chrono::milliseconds(1000));

    session.stop();
}

namespace {

// Torrent 3 piece x 16 byte, chia thành 3 file: piece 0 vắt a.txt/sub/b.txt, piece 1 nằm hẳn trong
// sub/b.txt, piece 2 vắt sub/b.txt/c.txt (có thêm 1 file dài 0 ở giữa).
struct MultiFileFixture {
    std::vector<std::string> pieces = {"0123456789ABCDEF", "GHIJKLMNOPQRSTUV", "WXYZabcdefghijkl"};
    TorrentFileMetadata meta;

    MultiFileFixture() {
        meta = makeMockMetadata("multi_root", pieces, 16);
        meta.announce = "";
        meta.files = {{"a.txt", 10}, {"sub/empty.txt", 0}, {"sub/b.txt", 30}, {"c.txt", 8}};
        for (const auto& f : meta.files) {
            meta.filePaths.push_back(f.path);
        }
        meta.infoRaw = "d4:name10:multi_root12:piece lengthi16e6:lengthi48ee";  // đủ để có info hash riêng
    }

    std::string payload() const { return pieces[0] + pieces[1] + pieces[2]; }

    // Ghi các file dưới root theo layout, chỉ lấy `limit` byte đầu của luồng.
    void writeTo(const std::filesystem::path& root, std::size_t limit) const {
        const std::string all = payload();
        std::size_t offset = 0;
        for (const auto& f : meta.files) {
            const auto path = root / f.path;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            const std::size_t end = std::min<std::size_t>(offset + f.length, limit);
            if (end > offset) {
                out.write(all.data() + offset, static_cast<std::streamsize>(end - offset));
            }
            offset += f.length;
        }
    }
};

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

void testSessionRateLimitsAndPauseResume() {
    std::cout << "[TEST] TorrentSession rate limits and pause/resume state..." << std::endl;
    const std::filesystem::path temp_dir = "./test_limits_pause";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("limits.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6902 + 90;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.max_download_rate = 5000;
    config.max_upload_rate = 7000;

    TorrentSession session(meta, config);
    CHECK_EQ(session.download_limit(), std::uint64_t(5000));
    CHECK_EQ(session.upload_limit(), std::uint64_t(7000));
    session.set_download_limit(0);
    session.set_upload_limit(123);
    CHECK_EQ(session.download_limit(), std::uint64_t(0));
    CHECK_EQ(session.upload_limit(), std::uint64_t(123));

    CHECK(session.start());
    CHECK(!session.is_paused());
    session.pause();
    CHECK(session.is_paused());
    CHECK_EQ(session.state(), SessionState::Paused);
    session.resume();
    CHECK(!session.is_paused());
    CHECK(session.state() != SessionState::Paused);
    session.stop();

    std::filesystem::remove_all(temp_dir, ec);
}

void testStateBridgePersistsAndRestoresTorrents() {
    std::cout << "[TEST] StateBridge saves torrent list, pause state and limits across restarts..." << std::endl;
    const std::filesystem::path temp_dir = "./test_state_persist";
    const std::filesystem::path state_dir = temp_dir / ".p2p_state";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("persist_me.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    meta.infoRaw = "d6:lengthi32e4:name14:persist_me.bin12:piece lengthi16e6:pieces40:" + meta.pieces[0] + meta.pieces[1] + "e";
    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6990;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.max_download_rate = 2048;
    config.max_upload_rate = 4096;

    auto& bridge = StateBridge::instance();
    bridge.setStateDir(state_dir);
    auto session = std::make_shared<TorrentSession>(meta, config);
    CHECK(session->start());
    session->pause();
    const std::string magnet = "magnet:?xt=urn:btih:" + session->info_hash_hex() + "&dn=persist_me.bin";
    bridge.addSession(session, magnet);
    bridge.persistState(session->info_hash_hex());

    auto saved = bridge.loadSavedTorrents();
    CHECK_EQ(saved.size(), std::size_t(1));
    if (saved.size() == 1) {
        CHECK_EQ(saved[0].metadata.name, std::string("persist_me.bin"));
        CHECK_EQ(saved[0].metadata.infoRaw, meta.infoRaw);
        CHECK_EQ(saved[0].magnet_uri, magnet);
        CHECK(saved[0].paused);
        CHECK_EQ(saved[0].download_limit, std::uint64_t(2048));
        CHECK_EQ(saved[0].upload_limit, std::uint64_t(4096));
    }

    // Đổi giới hạn rồi lưu lại: lần đọc sau thấy giá trị mới.
    session->set_download_limit(0);
    session->resume();
    bridge.persistState(session->info_hash_hex());
    saved = bridge.loadSavedTorrents();
    CHECK_EQ(saved.size(), std::size_t(1));
    if (saved.size() == 1) {
        CHECK(!saved[0].paused);
        CHECK_EQ(saved[0].download_limit, std::uint64_t(0));
    }

    // Torrent bị xóa thì file lưu cũng mất; file hỏng thì bị bỏ qua, không làm hỏng việc khôi phục.
    CHECK(bridge.removeSession(session->info_hash_hex()));
    CHECK_EQ(bridge.loadSavedTorrents().size(), std::size_t(0));
    {
        std::ofstream bad(state_dir / "garbage.torrent", std::ios::binary);
        bad << "not bencode";
    }
    CHECK_EQ(bridge.loadSavedTorrents().size(), std::size_t(0));

    bridge.setStateDir({});
    std::filesystem::remove_all(temp_dir, ec);
}

void testStateBridgeSanitizesMagnetAndPersistsDiscoveryFlags() {
    std::cout << "[TEST] StateBridge strips CR/LF from magnet and persists tracker/dht/lsd flags..." << std::endl;
    const std::filesystem::path temp_dir = "./test_state_flags";
    const std::filesystem::path state_dir = temp_dir / ".p2p_state";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("flags_me.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    meta.infoRaw = "d6:lengthi32e4:name12:flags_me.bin12:piece lengthi16e6:pieces40:" + meta.pieces[0] + meta.pieces[1] + "e";
    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6992;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;

    auto& bridge = StateBridge::instance();
    bridge.setStateDir(state_dir);
    auto session = std::make_shared<TorrentSession>(meta, config);
    CHECK(session->start());
    const std::string hash = session->info_hash_hex();
    // Magnet độc hại: cố chèn dòng paused/down/skip/dht giả vào file .meta.
    const std::string evil = "magnet:?xt=urn:btih:" + hash + "\npaused=1\r\ndown=999\nskip=0\ndht=1";
    SessionFlags flags;
    flags.enable_tracker = false;
    flags.enable_dht = false;
    flags.enable_lsd = true;
    bridge.addSession(session, evil, flags);
    bridge.persistState(hash);

    // Không khóa lạ nào ngoài danh sách cho phép, và mỗi khóa đúng 1 lần.
    std::map<std::string, int> keyCounts;
    {
        std::ifstream in(state_dir / (hash + ".meta"));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq != std::string::npos) ++keyCounts[line.substr(0, eq)];
        }
    }
    for (const auto& [key, count] : keyCounts) {
        CHECK(key == "magnet" || key == "paused" || key == "down" || key == "up" || key == "skip" ||
              key == "tracker" || key == "dht" || key == "lsd");
        CHECK_EQ(count, 1);
    }

    auto saved = bridge.loadSavedTorrents();
    CHECK_EQ(saved.size(), std::size_t(1));
    if (saved.size() == 1) {
        CHECK(!saved[0].paused);
        CHECK_EQ(saved[0].download_limit, std::uint64_t(0));
        CHECK(saved[0].skipped_files.empty());
        CHECK(saved[0].magnet_uri.find('\n') == std::string::npos);
        CHECK(saved[0].magnet_uri.find('\r') == std::string::npos);
        CHECK(!saved[0].enable_tracker);
        CHECK(!saved[0].enable_dht);
        CHECK(saved[0].enable_lsd);
    }

    // File .meta cũ (không có tracker/dht/lsd) vẫn đọc được và mặc định bật cả ba.
    {
        std::ofstream legacy(state_dir / (hash + ".meta"), std::ios::trunc);
        legacy << "magnet=magnet:?xt=urn:btih:" << hash << "\npaused=0\ndown=0\nup=0\n";
    }
    saved = bridge.loadSavedTorrents();
    CHECK_EQ(saved.size(), std::size_t(1));
    if (saved.size() == 1) {
        CHECK(saved[0].enable_tracker);
        CHECK(saved[0].enable_dht);
        CHECK(saved[0].enable_lsd);
    }

    CHECK(bridge.removeSession(hash));
    bridge.setStateDir({});
    std::filesystem::remove_all(temp_dir, ec);
}

void testStateBridgePickerStrategyPerTorrentAndForAll() {
    std::cout << "[TEST] StateBridge sets piece strategy per torrent and for all, keeps pipeline depth..." << std::endl;
    const std::filesystem::path temp_dir = "./test_picker_strategy";
    const std::filesystem::path state_dir = temp_dir / ".p2p_state";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.pipeline_depth = 12;  // khác mặc định 4 của picker để bắt lỗi rơi về cấu hình mặc định

    auto& bridge = StateBridge::instance();
    bridge.setStateDir(state_dir);
    CHECK_EQ(bridge.defaultPickerStrategy(), std::string("rarest_first"));

    auto sessionA = std::make_shared<TorrentSession>(makeMockMetadata("strategy_a.bin", {"PieceA1", "PieceA2"}, 7), config);
    auto sessionB = std::make_shared<TorrentSession>(makeMockMetadata("strategy_b.bin", {"PieceB1", "PieceB2"}, 7), config);
    bridge.addSession(sessionA, "magnet:?xt=urn:btih:a");
    bridge.addSession(sessionB, "magnet:?xt=urn:btih:b");
    const std::string hashA = sessionA->info_hash_hex();
    const std::string hashB = sessionB->info_hash_hex();
    CHECK_EQ(sessionA->piece_pipeline_depth(), std::size_t(12));

    // Từng torrent: chỉ torrent được chọn đổi, pipeline_depth không tụt về 4.
    CHECK(bridge.setPickerStrategy(hashA, "sequential"));
    CHECK_EQ(bridge.getPickerStrategy(hashA), std::string("sequential"));
    CHECK_EQ(bridge.getPickerStrategy(hashB), std::string("rarest_first"));
    CHECK_EQ(sessionA->piece_pipeline_depth(), std::size_t(12));
    CHECK(bridge.setPickerStrategy(hashA, "rarest_first"));
    CHECK_EQ(sessionA->piece_pipeline_depth(), std::size_t(12));

    // Tên lạ hoặc torrent không tồn tại: từ chối, không đổi gì.
    CHECK(!bridge.setPickerStrategy(hashA, "bogus"));
    CHECK_EQ(bridge.getPickerStrategy(hashA), std::string("rarest_first"));
    CHECK(!bridge.setPickerStrategy("0000000000000000000000000000000000000000", "sequential"));

    // Tất cả: đổi mọi torrent đang có và đặt mặc định.
    CHECK_EQ(bridge.setAllPickerStrategies("sequential"), 2);
    CHECK_EQ(bridge.getPickerStrategy(hashA), std::string("sequential"));
    CHECK_EQ(bridge.getPickerStrategy(hashB), std::string("sequential"));
    CHECK_EQ(bridge.defaultPickerStrategy(), std::string("sequential"));
    CHECK_EQ(sessionB->piece_pipeline_depth(), std::size_t(12));
    CHECK_EQ(bridge.setAllPickerStrategies("bogus"), -1);
    CHECK_EQ(bridge.defaultPickerStrategy(), std::string("sequential"));

    // Torrent thêm sau nhận chiến lược mặc định.
    auto sessionC = std::make_shared<TorrentSession>(makeMockMetadata("strategy_c.bin", {"PieceC1", "PieceC2"}, 7), config);
    bridge.addSession(sessionC, "magnet:?xt=urn:btih:c");
    CHECK_EQ(bridge.getPickerStrategy(sessionC->info_hash_hex()), std::string("sequential"));
    CHECK_EQ(sessionC->piece_pipeline_depth(), std::size_t(12));

    // Mặc định được lưu: đọc lại thư mục trạng thái vẫn thấy "sequential".
    bridge.setStateDir(state_dir);
    CHECK_EQ(bridge.defaultPickerStrategy(), std::string("sequential"));

    CHECK_EQ(bridge.setAllPickerStrategies("rarest_first"), 3);
    CHECK_EQ(bridge.defaultPickerStrategy(), std::string("rarest_first"));

    bridge.removeSession(sessionA->info_hash_hex());
    bridge.removeSession(sessionB->info_hash_hex());
    bridge.removeSession(sessionC->info_hash_hex());
    bridge.setStateDir({});
    std::filesystem::remove_all(temp_dir, ec);
}

void testDownloadQueueLimitsActiveTorrents() {
    std::cout << "[TEST] download queue keeps at most N torrents downloading and promotes waiting ones..." << std::endl;
    const std::filesystem::path temp_dir = "./test_queue";
    const std::filesystem::path state_dir = temp_dir / ".p2p_state";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto& bridge = StateBridge::instance();
    bridge.setStateDir(state_dir);
    bridge.setMaxActiveDownloads(0);

    std::vector<std::shared_ptr<TorrentSession>> sessions;
    for (int i = 0; i < 3; ++i) {
        const std::string name = std::string("queue_") + static_cast<char>('a' + i) + ".bin";
        auto meta = makeMockMetadata(name, {"AlphaPiece123456", "BetaPiece1234567"}, 16);
        meta.infoRaw = "d6:lengthi32e4:name" + std::to_string(name.size()) + ":" + name +
                       "12:piece lengthi16e6:pieces40:" + meta.pieces[0] + meta.pieces[1] + "e";
        TorrentSessionConfig config;
        config.download_dir = temp_dir;
        config.listen_port = static_cast<std::uint16_t>(6991 + i);
        config.enable_lsd = false;
        config.enable_dht = false;
        config.enable_tracker = false;
        config.enable_port_mapping = false;
        config.enable_utp = false;
        auto session = std::make_shared<TorrentSession>(meta, config);
        CHECK(session->start());
        sessions.push_back(session);
    }
    const auto hashOf = [&](int i) { return sessions[static_cast<std::size_t>(i)]->info_hash_hex(); };
    const auto paused = [&](int i) { return sessions[static_cast<std::size_t>(i)]->is_paused(); };

    bridge.setMaxActiveDownloads(1);
    for (auto& session : sessions) bridge.addSession(session, "magnet:?xt=urn:btih:" + session->info_hash_hex());
    CHECK_EQ(bridge.maxActiveDownloads(), std::size_t(1));
    // Chỉ torrent thêm đầu tiên chạy; hai cái sau xếp hàng.
    CHECK(!paused(0) && !bridge.isQueued(hashOf(0)));
    CHECK(paused(1) && bridge.isQueued(hashOf(1)));
    CHECK(paused(2) && bridge.isQueued(hashOf(2)));

    // JSON báo trạng thái "Queued" và không coi là người dùng tự tạm dừng.
    const std::string json = bridge.toJsonString();
    CHECK(json.find("\"state\":\"Queued\"") != std::string::npos);
    CHECK(json.find("\"queued\":true") != std::string::npos);

    // Người dùng dừng torrent đang chạy: chỗ trống đi cho torrent chờ lâu nhất (B), C vẫn chờ.
    CHECK(bridge.userPause(hashOf(0)));
    CHECK(paused(0) && !bridge.isQueued(hashOf(0)));  // dừng thủ công, KHÔNG xếp hàng
    CHECK(!paused(1) && !bridge.isQueued(hashOf(1)));
    CHECK(paused(2) && bridge.isQueued(hashOf(2)));

    // Người dùng tiếp tục torrent đang xếp hàng (C): nó lên đầu và đẩy B (mới nhất đang chạy) về hàng.
    CHECK(bridge.userResume(hashOf(2)));
    CHECK(!paused(2) && !bridge.isQueued(hashOf(2)));
    CHECK(paused(1) && bridge.isQueued(hashOf(1)));
    // A (dừng thủ công) vẫn không tự chạy lại.
    CHECK(paused(0) && !bridge.isQueued(hashOf(0)));

    // Xoá torrent đang chạy: B được chạy.
    CHECK(bridge.removeSession(hashOf(2)));
    CHECK(!paused(1) && !bridge.isQueued(hashOf(1)));

    // Giá trị giới hạn được lưu và đọc lại; trạng thái "xếp hàng" không bị lưu thành "người dùng dừng".
    bridge.setMaxActiveDownloads(3);
    CHECK(!paused(1));
    bridge.setStateDir(state_dir);
    CHECK_EQ(bridge.maxActiveDownloads(), std::size_t(3));

    // Tắt giới hạn: không còn torrent nào bị xếp hàng.
    bridge.setMaxActiveDownloads(0);
    CHECK(!bridge.isQueued(hashOf(0)) && !bridge.isQueued(hashOf(1)));
    CHECK(bridge.removeSession(hashOf(0)));
    CHECK(bridge.removeSession(hashOf(1)));
    sessions[2]->stop();

    bridge.setStateDir({});
    std::filesystem::remove_all(temp_dir, ec);
}

// ---- BEP 6 Fast Extension ----

void testAllowedFastSetMatchesBep6Vectors() {
    std::cout << "[TEST] allowed fast set matches the BEP 6 reference vectors..." << std::endl;
    const std::string hash(20, '\xaa');
    const std::uint32_t ip = (80u << 24) | (4u << 16) | (4u << 8) | 200u;
    const auto seven = compute_allowed_fast_set(ip, hash, 1313, 7);
    const std::vector<std::uint32_t> expectSeven = {1059, 431, 808, 1217, 287, 376, 1188};
    CHECK(seven == expectSeven);
    const auto nine = compute_allowed_fast_set(ip, hash, 1313, 9);
    const std::vector<std::uint32_t> expectNine = {1059, 431, 808, 1217, 287, 376, 1188, 353, 508};
    CHECK(nine == expectNine);
    // Chỉ 24 bit đầu của IP được tính (cùng /24 -> cùng tập), tập nhỏ hơn số piece thì bị cắt, không trùng.
    CHECK(compute_allowed_fast_set(ip | 0x37u, hash, 1313, 7) == expectSeven);
    CHECK(compute_allowed_fast_set(ip + 0x100u, hash, 1313, 7) != expectSeven);
    const auto tiny = compute_allowed_fast_set(ip, hash, 3, 10);
    CHECK_EQ(tiny.size(), std::size_t(3));
    CHECK(compute_allowed_fast_set(ip, hash, 0, 10).empty());
    CHECK(compute_allowed_fast_set(ip, std::string("short"), 10, 3).empty());
}

namespace {

struct RawFrame {
    bool keep_alive = false;
    std::uint8_t id = 0;
    std::vector<std::uint8_t> payload;
};

void setReceiveTimeout(asio::ip::tcp::socket& socket, int ms) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(ms);
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval value{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

bool readExact(asio::ip::tcp::socket& socket, void* out, std::size_t size) {
    asio::error_code ec;
    asio::read(socket, asio::buffer(out, size), ec);
    return !ec;
}

bool readFrame(asio::ip::tcp::socket& socket, RawFrame& frame) {
    std::uint8_t header[4];
    if (!readExact(socket, header, 4)) return false;
    const std::uint32_t length = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16) |
                                 (std::uint32_t(header[2]) << 8) | header[3];
    frame = RawFrame{};
    if (length == 0) {
        frame.keep_alive = true;
        return true;
    }
    if (length > 1024 * 1024) return false;
    std::vector<std::uint8_t> body(length);
    if (!readExact(socket, body.data(), length)) return false;
    frame.id = body[0];
    frame.payload.assign(body.begin() + 1, body.end());
    return true;
}

std::uint32_t beAt(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::string rawMessage(std::uint8_t id, const std::vector<std::uint32_t>& words, const std::string& tail = "") {
    std::string body;
    body.push_back(static_cast<char>(id));
    for (std::uint32_t word : words) {
        for (int shift = 24; shift >= 0; shift -= 8) body.push_back(static_cast<char>((word >> shift) & 0xFF));
    }
    body += tail;
    std::string frame;
    const std::uint32_t length = static_cast<std::uint32_t>(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((length >> shift) & 0xFF));
    return frame + body;
}

// Bắt tay thô với session: trả handshake của session (reserved + info hash), bit fast tuỳ chọn.
bool rawHandshake(asio::ip::tcp::socket& socket, const std::string& infoHashHex, bool withFast,
                  std::array<std::uint8_t, 8>& remoteReserved) {
    std::string handshake;
    handshake.push_back(static_cast<char>(19));
    handshake += "BitTorrent protocol";
    std::string reserved(8, '\0');
    if (withFast) reserved[7] = static_cast<char>(0x04);
    handshake += reserved;
    for (std::size_t i = 0; i + 1 < infoHashHex.size(); i += 2) {
        handshake.push_back(static_cast<char>(std::stoi(infoHashHex.substr(i, 2), nullptr, 16)));
    }
    handshake += "-RAWFST-abcdefghijkl";
    asio::write(socket, asio::buffer(handshake));
    std::uint8_t reply[68];
    if (!readExact(socket, reply, sizeof(reply))) return false;
    std::copy(reply + 20, reply + 28, remoteReserved.begin());
    return true;
}

}  // namespace

void testFastExtensionSeederSendsHaveAllAllowedFastAndRejects() {
    std::cout << "[TEST] Fast Extension: seeder sends HaveAll + AllowedFast, rejects choked requests, serves allowed ones..." << std::endl;
    const std::filesystem::path temp_dir = "./test_fast_seeder";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    std::vector<std::string> pieces;
    for (int i = 0; i < 20; ++i) pieces.push_back(std::string(16384, static_cast<char>('a' + i)));
    auto meta = makeMockMetadata("fast_seed.bin", pieces, 16384);
    {
        std::ofstream out(temp_dir / meta.name, std::ios::binary);
        for (const auto& piece : pieces) out << piece;
    }

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7230;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.enable_port_mapping = false;
    config.enable_utp = false;
    TorrentSession session(meta, config);
    CHECK(session.start());
    CHECK(session.is_complete());

    const std::string hexHash = session.info_hash_hex();
    std::string rawHash;
    for (std::size_t i = 0; i + 1 < hexHash.size(); i += 2) {
        rawHash.push_back(static_cast<char>(std::stoi(hexHash.substr(i, 2), nullptr, 16)));
    }
    const auto expected = compute_allowed_fast_set((127u << 24) | 1u, rawHash, 20, 10);
    const std::set<std::uint32_t> expectedSet(expected.begin(), expected.end());
    std::uint32_t allowedPiece = *expectedSet.begin();
    std::uint32_t forbiddenPiece = 0;
    while (expectedSet.count(forbiddenPiece) != 0) ++forbiddenPiece;

    {
        // --- Peer có Fast Extension ---
        asio::io_context ioc;
        asio::ip::tcp::socket peer(ioc);
        peer.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7230));
        setReceiveTimeout(peer, 8000);  // worker của peer vào chỉ bắt đầu sau một nhịp vòng điều phối
        std::array<std::uint8_t, 8> reserved{};
        CHECK(rawHandshake(peer, hexHash, true, reserved));
        CHECK((reserved[7] & 0x04) != 0);  // session cũng quảng bá Fast Extension

        bool sawHaveAll = false;
        bool sawBitfield = false;
        std::set<std::uint32_t> allowed;
        RawFrame frame;
        while (readFrame(peer, frame)) {
            if (frame.keep_alive) continue;
            if (frame.id == 14) sawHaveAll = true;
            if (frame.id == 5) sawBitfield = true;
            if (frame.id == 17 && frame.payload.size() == 4) allowed.insert(beAt(frame.payload, 0));
            if (allowed.size() >= 10 && sawHaveAll) break;
        }
        CHECK(sawHaveAll);
        CHECK(!sawBitfield);
        CHECK(allowed == expectedSet);  // đúng tập chuẩn tính từ IP 127.0.0.1 và info hash

        // Đang bị choke (chưa bao giờ gửi Interested): piece ngoài tập allowed bị Reject, rõ ràng.
        asio::write(peer, asio::buffer(rawMessage(6, {forbiddenPiece, 0, 16384})));
        bool rejected = false;
        while (readFrame(peer, frame)) {
            if (frame.keep_alive) continue;
            if (frame.id == 16 && frame.payload.size() == 12) {
                rejected = beAt(frame.payload, 0) == forbiddenPiece && beAt(frame.payload, 4) == 0 &&
                           beAt(frame.payload, 8) == 16384;
                break;
            }
            CHECK(frame.id != 7);  // không được phục vụ dữ liệu cho piece bị cấm
        }
        CHECK(rejected);

        // Piece trong tập allowed được phục vụ ngay cả khi vẫn bị choke.
        asio::write(peer, asio::buffer(rawMessage(6, {allowedPiece, 0, 16384})));
        bool served = false;
        while (readFrame(peer, frame)) {
            if (frame.keep_alive) continue;
            if (frame.id == 7 && frame.payload.size() == 8 + 16384) {
                served = beAt(frame.payload, 0) == allowedPiece && beAt(frame.payload, 4) == 0 &&
                         frame.payload[8] == static_cast<std::uint8_t>('a' + allowedPiece);
                break;
            }
        }
        CHECK(served);

        // Piece không tồn tại (ngoài phạm vi) cũng được Reject chứ không im lặng.
        asio::write(peer, asio::buffer(rawMessage(6, {9999, 0, 16384})));
        bool rejectedMissing = false;
        while (readFrame(peer, frame)) {
            if (!frame.keep_alive && frame.id == 16) {
                rejectedMissing = beAt(frame.payload, 0) == 9999;
                break;
            }
        }
        CHECK(rejectedMissing);
        asio::error_code ignored;
        peer.close(ignored);
    }
    {
        // --- Peer KHÔNG có Fast Extension: giữ nguyên hành vi cũ (Bitfield, không Allowed Fast, không Reject) ---
        asio::io_context ioc;
        asio::ip::tcp::socket peer(ioc);
        peer.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7230));
        setReceiveTimeout(peer, 3500);
        std::array<std::uint8_t, 8> reserved{};
        CHECK(rawHandshake(peer, hexHash, false, reserved));
        bool sawBitfield = false;
        RawFrame frame;
        asio::write(peer, asio::buffer(rawMessage(6, {forbiddenPiece, 0, 16384})));
        while (readFrame(peer, frame)) {
            if (frame.keep_alive) continue;
            CHECK(frame.id != 14);
            CHECK(frame.id != 17);
            CHECK(frame.id != 16);
            CHECK(frame.id != 7);  // vẫn bị choke: không được phục vụ
            if (frame.id == 5) sawBitfield = true;
        }
        CHECK(sawBitfield);
        asio::error_code ignored;
        peer.close(ignored);
    }
    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testFastExtensionLeecherDownloadsAllowedPieceWhileChoked() {
    std::cout << "[TEST] Fast Extension: leecher requests an allowed-fast piece while still choked..." << std::endl;
    const std::filesystem::path temp_dir = "./test_fast_leecher";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    std::vector<std::string> pieces;
    for (int i = 0; i < 6; ++i) pieces.push_back(std::string(16384, static_cast<char>('A' + i)));
    auto meta = makeMockMetadata("fast_leech.bin", pieces, 16384);

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7231;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.enable_port_mapping = false;
    config.enable_utp = false;
    TorrentSession session(meta, config);
    CHECK(session.start());

    asio::io_context ioc;
    asio::ip::tcp::socket remote(ioc);
    remote.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7231));
    setReceiveTimeout(remote, 8000);
    std::array<std::uint8_t, 8> reserved{};
    CHECK(rawHandshake(remote, session.info_hash_hex(), true, reserved));
    CHECK((reserved[7] & 0x04) != 0);

    // Peer giả có đủ dữ liệu (HaveAll) nhưng KHÔNG unchoke, chỉ cho phép piece 3 qua Allowed Fast.
    asio::write(remote, asio::buffer(rawMessage(14, {})));
    asio::write(remote, asio::buffer(rawMessage(17, {3})));

    bool sawInterested = false;
    bool requestedAllowed = false;
    RawFrame frame;
    while (readFrame(remote, frame)) {
        if (frame.keep_alive) continue;
        if (frame.id == 2) sawInterested = true;
        if (frame.id == 6 && frame.payload.size() == 12) {
            CHECK_EQ(beAt(frame.payload, 0), std::uint32_t(3));  // chỉ được xin piece allowed fast khi bị choke
            if (beAt(frame.payload, 0) == 3 && beAt(frame.payload, 4) == 0) {
                requestedAllowed = true;
                break;
            }
        }
    }
    CHECK(sawInterested);
    CHECK(requestedAllowed);

    // Trả dữ liệu đúng: piece 3 hoàn tất dù ta chưa từng unchoke / họ chưa từng được unchoke.
    asio::write(remote, asio::buffer(rawMessage(7, {3, 0}, pieces[3])));
    bool completed = false;
    for (int i = 0; i < 50 && !completed; ++i) {
        completed = session.stats().completed_pieces == 1;
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(completed);

    // Hết piece được phép: không được xin thêm piece nào khác trong lúc vẫn bị choke.
    setReceiveTimeout(remote, 700);
    bool askedOther = false;
    while (readFrame(remote, frame)) {
        if (!frame.keep_alive && frame.id == 6) askedOther = true;
    }
    CHECK(!askedOther);

    // Peer Reject 1 piece allowed khác: không bị xin lại liên tục (không vòng lặp request/reject).
    asio::write(remote, asio::buffer(rawMessage(17, {5})));
    setReceiveTimeout(remote, 1500);
    bool askedFive = false;
    while (readFrame(remote, frame)) {
        if (!frame.keep_alive && frame.id == 6 && beAt(frame.payload, 0) == 5) {
            askedFive = true;
            break;
        }
    }
    CHECK(askedFive);
    asio::write(remote, asio::buffer(rawMessage(16, {5, 0, 16384})));
    setReceiveTimeout(remote, 900);
    int again = 0;
    while (readFrame(remote, frame)) {
        if (!frame.keep_alive && frame.id == 6 && beAt(frame.payload, 0) == 5) ++again;
    }
    CHECK_EQ(again, 0);

    asio::error_code ignored;
    remote.close(ignored);
    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testGlobalBandwidthLimitIsSharedAndPersisted() {
    std::cout << "[TEST] global bandwidth limit is shared by sessions and survives restarts..." << std::endl;
    const std::filesystem::path temp_dir = "./test_global_limit";
    const std::filesystem::path state_dir = temp_dir / ".p2p_state";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto& bridge = StateBridge::instance();
    bridge.setStateDir(state_dir);
    bridge.setGlobalLimits(0, 0);
    CHECK_EQ(bridge.globalDownloadLimit(), std::uint64_t(0));

    bridge.setGlobalLimits(5 * 1024, 6 * 1024);
    CHECK_EQ(bridge.globalDownloadLimit(), std::uint64_t(5 * 1024));
    CHECK_EQ(bridge.globalUploadLimit(), std::uint64_t(6 * 1024));

    // Mọi session tạo sau đó dùng CHUNG một cặp bucket (không phải mỗi session một bản sao).
    TorrentSessionConfig first;
    TorrentSessionConfig second;
    bridge.applyGlobalLimiters(first);
    bridge.applyGlobalLimiters(second);
    CHECK(first.global_download_limiter != nullptr);
    CHECK(first.global_download_limiter == second.global_download_limiter);
    CHECK(first.global_upload_limiter == second.global_upload_limiter);
    CHECK_EQ(first.global_download_limiter->rate(), std::uint64_t(5 * 1024));
    // Đổi giới hạn có hiệu lực ngay với session đã tạo.
    bridge.setGlobalLimits(7 * 1024, 0);
    CHECK_EQ(first.global_download_limiter->rate(), std::uint64_t(7 * 1024));
    CHECK_EQ(first.global_upload_limiter->rate(), std::uint64_t(0));
    bridge.setGlobalLimits(5 * 1024, 6 * 1024);

    // Sang thư mục trạng thái khác (rỗng) rồi quay lại: giá trị được đọc lại từ đĩa.
    bridge.setStateDir(temp_dir / "other_state");
    CHECK_EQ(bridge.globalDownloadLimit(), std::uint64_t(0));
    bridge.setStateDir(state_dir);
    CHECK_EQ(bridge.globalDownloadLimit(), std::uint64_t(5 * 1024));
    CHECK_EQ(bridge.globalUploadLimit(), std::uint64_t(6 * 1024));

    bridge.setGlobalLimits(0, 0);
    bridge.setStateDir({});
    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionMultiFileExistingSeeding() {
    std::cout << "[TEST] TorrentSession multi-file existing data (Seeder mode)...\n";
    const std::filesystem::path temp_dir = "./test_multifile_seeder";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    MultiFileFixture fx;
    fx.writeTo(temp_dir / fx.meta.name, 48);

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6898;
    config.enable_lsd = false;

    TorrentSession session(fx.meta, config);
    CHECK_EQ(session.stats().completed_pieces, std::size_t(3));
    CHECK(session.is_complete());
    CHECK_EQ(session.stats().state, SessionState::Seeding);

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionMultiFilePartialDataResumes() {
    std::cout << "[TEST] TorrentSession multi-file partial data keeps only valid pieces...\n";
    const std::filesystem::path temp_dir = "./test_multifile_partial";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    MultiFileFixture fx;
    fx.writeTo(temp_dir / fx.meta.name, 20);  // chỉ piece 0 đủ byte; c.txt chỉ là file rỗng

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6899;
    config.enable_lsd = false;

    TorrentSession session(fx.meta, config);
    CHECK_EQ(session.stats().completed_pieces, std::size_t(1));
    CHECK(!session.is_complete());
    CHECK_EQ(session.stats().state, SessionState::Downloading);

    std::filesystem::remove_all(temp_dir, ec);
}

void testSessionMultiFileLoopbackDownload() {
    std::cout << "[TEST] TorrentSession multi-file loopback download writes every file...\n";
    const std::filesystem::path temp_dir = "./test_multifile_dl";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    MultiFileFixture fx;
    const auto& meta = fx.meta;

    asio::io_context seeder_ioc;
    asio::ip::tcp::acceptor acceptor(
        seeder_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 6900));

    InfoHash expected_hash{};
    const std::string raw_hash = p2p::sha1Digest(meta.infoRaw);
    std::memcpy(expected_hash.data(), raw_hash.data(), 20);

    std::thread seeder_thread([&]() {
        try {
            asio::ip::tcp::socket sock = acceptor.accept();
            PeerConnection seeder_peer(std::move(sock), std::chrono::seconds(5));
            seeder_peer.receive_handshake();
            PeerHandshake reply_hs;
            reply_hs.info_hash = expected_hash;
            reply_hs.peer_id.fill(0x55);
            seeder_peer.send_handshake(reply_hs);

            auto msg = seeder_peer.receive_message();  // bitfield của client
            seeder_peer.send_bitfield(std::vector<bool>{true, true, true});

            msg = seeder_peer.receive_message();
            while (msg.id == static_cast<Byte>(PeerMessageId::Port)) {
                msg = seeder_peer.receive_message();
            }
            seeder_peer.send_unchoke();

            int served = 0;
            while (served < 3) {
                msg = seeder_peer.receive_message();
                if (msg.id == static_cast<Byte>(PeerMessageId::Request)) {
                    auto req = peer_protocol::parse_request(msg);
                    if (req.index < 3) {
                        const std::string& data = fx.pieces[req.index];
                        seeder_peer.send_piece(req.index, 0, reinterpret_cast<const Byte*>(data.data()), data.size());
                        ++served;
                    }
                }
            }
            while (seeder_peer.is_open()) {
                seeder_peer.receive_message();
            }
        } catch (const std::exception&) {
            // Kết nối đóng khi session dừng: kết thúc luồng seeder.
        }
    });

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 6901;
    config.enable_lsd = false;

    TorrentSession session(meta, config);
    session.start();
    session.add_peer("127.0.0.1", 6900);

    const auto start_time = std::chrono::steady_clock::now();
    while (!session.is_complete() &&
           std::chrono::steady_clock::now() - start_time < std::chrono::seconds(8)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(session.is_complete());
    session.stop();
    if (seeder_thread.joinable()) {
        seeder_thread.join();
    }

    const auto root = temp_dir / meta.name;
    const std::string all = fx.payload();
    CHECK_EQ(slurp(root / "a.txt"), all.substr(0, 10));
    CHECK(std::filesystem::exists(root / "sub/empty.txt"));
    CHECK_EQ(std::filesystem::file_size(root / "sub/empty.txt"), std::uintmax_t(0));
    CHECK_EQ(slurp(root / "sub/b.txt"), all.substr(10, 30));
    CHECK_EQ(slurp(root / "c.txt"), all.substr(40, 8));
    CHECK(!std::filesystem::exists(temp_dir / (meta.name + ".part")));  // multi-file không dùng .part

    std::filesystem::remove_all(temp_dir, ec);
}

void testFileSelectionMapsToPiecesAndValidatesInput() {
    std::cout << "[TEST] File selection maps files to pieces and rejects invalid input..." << std::endl;
    const std::filesystem::path temp_dir = "./test_select_basic";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    MultiFileFixture fx;  // a.txt[0,10) empty b.txt[10,40) c.txt[40,48); piece 16 byte: p0=[0,16) p1=[16,32) p2=[32,48)
    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7002;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    TorrentSession session(fx.meta, config);

    CHECK_EQ(session.stats().wanted_bytes, std::uint64_t(48));
    CHECK_EQ(session.file_selection().size(), std::size_t(4));

    // Chỉ c.txt (byte 40-47) -> chỉ piece 2.
    CHECK(session.set_file_selection({false, false, false, true}));
    CHECK_EQ(session.stats().wanted_bytes, std::uint64_t(16));
    CHECK_EQ(session.stats().left_bytes, std::uint64_t(16));
    CHECK_EQ(session.stats().total_bytes, std::uint64_t(48));
    auto files = session.files_status();
    CHECK_EQ(files.size(), std::size_t(4));
    CHECK(!files[0].wanted && !files[2].wanted && files[3].wanted);

    // Chỉ a.txt (byte 0-9) -> chỉ piece 0. Bỏ b.txt thì piece 1 không cần.
    CHECK(session.set_file_selection({true, false, false, false}));
    CHECK_EQ(session.stats().wanted_bytes, std::uint64_t(16));

    // b.txt (byte 10-39) -> piece 0,1,2 (đứt 2 đầu vắt qua file bên cạnh).
    CHECK(session.set_file_selection({false, false, true, false}));
    CHECK_EQ(session.stats().wanted_bytes, std::uint64_t(48));

    // Đầu vào sai bị từ chối và không đổi gì.
    CHECK(!session.set_file_selection({true, true}));                        // sai số lượng
    CHECK(!session.set_file_selection({false, false, false, false}));        // bỏ hết
    CHECK_EQ(session.stats().wanted_bytes, std::uint64_t(48));
    CHECK(session.file_selection()[2]);

    std::filesystem::remove_all(temp_dir, ec);
}

void testFileSelectionWithExistingDataChangesSeedingState() {
    std::cout << "[TEST] File selection with existing data switches Seeding/Downloading..." << std::endl;
    const std::filesystem::path temp_dir = "./test_select_existing";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    MultiFileFixture fx;
    fx.writeTo(temp_dir / fx.meta.name, 16);  // chỉ piece 0 có dữ liệu đúng

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7003;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    TorrentSession session(fx.meta, config);
    CHECK_EQ(session.stats().completed_pieces, std::size_t(1));
    CHECK_EQ(session.stats().state, SessionState::Downloading);

    auto files = session.files_status();
    CHECK_EQ(files[0].completed_bytes, std::uint64_t(10));  // a.txt đủ
    CHECK_EQ(files[2].completed_bytes, std::uint64_t(6));   // b.txt: byte 10-15 nằm trong piece 0
    CHECK_EQ(files[3].completed_bytes, std::uint64_t(0));

    // Chỉ chọn a.txt: đã có đủ piece 0 -> xong, chuyển sang Seeding và 100%.
    CHECK(session.set_file_selection({true, false, false, false}));
    CHECK(session.is_complete());
    CHECK_EQ(session.stats().left_bytes, std::uint64_t(0));
    CHECK_EQ(session.stats().progress_percent, 100.0);

    // Chọn thêm c.txt: lại thiếu piece 2.
    CHECK(session.set_file_selection({true, false, false, true}));
    CHECK(!session.is_complete());
    CHECK_EQ(session.stats().left_bytes, std::uint64_t(16));

    std::filesystem::remove_all(temp_dir, ec);
}

void testFileSelectionLoopbackDownloadsOnlySelectedPieces() {
    std::cout << "[TEST] File selection downloads only selected pieces over the wire..." << std::endl;
    const std::filesystem::path temp_dir = "./test_select_dl";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    MultiFileFixture fx;
    const auto& meta = fx.meta;

    asio::io_context seeder_ioc;
    asio::ip::tcp::acceptor acceptor(
        seeder_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7004));
    InfoHash expected_hash{};
    const std::string raw_hash = p2p::sha1Digest(meta.infoRaw);
    std::memcpy(expected_hash.data(), raw_hash.data(), 20);

    std::vector<std::uint32_t> requested_pieces;
    std::thread seeder_thread([&]() {
        try {
            asio::ip::tcp::socket sock = acceptor.accept();
            PeerConnection seeder_peer(std::move(sock), std::chrono::seconds(5));
            seeder_peer.receive_handshake();
            PeerHandshake reply_hs;
            reply_hs.info_hash = expected_hash;
            reply_hs.peer_id.fill(0x66);
            seeder_peer.send_handshake(reply_hs);
            auto msg = seeder_peer.receive_message();
            seeder_peer.send_bitfield(std::vector<bool>{true, true, true});
            msg = seeder_peer.receive_message();
            while (msg.id == static_cast<Byte>(PeerMessageId::Port)) {
                msg = seeder_peer.receive_message();
            }
            seeder_peer.send_unchoke();
            while (seeder_peer.is_open()) {
                msg = seeder_peer.receive_message();
                if (msg.id == static_cast<Byte>(PeerMessageId::Request)) {
                    auto req = peer_protocol::parse_request(msg);
                    requested_pieces.push_back(req.index);
                    if (req.index < 3) {
                        const std::string& data = fx.pieces[req.index];
                        seeder_peer.send_piece(req.index, 0, reinterpret_cast<const Byte*>(data.data()), data.size());
                    }
                }
            }
        } catch (const std::exception&) {
        }
    });

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7005;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    TorrentSession session(meta, config);
    CHECK(session.set_file_selection({false, false, false, true}));  // chỉ c.txt -> chỉ piece 2
    session.start();
    session.add_peer("127.0.0.1", 7004);

    const auto start_time = std::chrono::steady_clock::now();
    while (!session.is_complete() &&
           std::chrono::steady_clock::now() - start_time < std::chrono::seconds(8)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(session.is_complete());
    CHECK_EQ(session.state(), SessionState::Seeding);
    CHECK_EQ(session.stats().progress_percent, 100.0);
    CHECK_EQ(session.stats().completed_pieces, std::size_t(1));  // chỉ 1/3 piece được tải
    session.stop();
    if (seeder_thread.joinable()) {
        seeder_thread.join();
    }

    for (std::uint32_t index : requested_pieces) {
        CHECK_EQ(index, std::uint32_t(2));  // không bao giờ xin piece ngoài phần được chọn
    }
    const auto root = temp_dir / meta.name;
    CHECK_EQ(slurp(root / "c.txt"), fx.payload().substr(40, 8));
    CHECK(!std::filesystem::exists(root / "a.txt"));          // file bị bỏ không được tạo
    CHECK(!std::filesystem::exists(root / "sub/empty.txt"));

    std::filesystem::remove_all(temp_dir, ec);
}

namespace {

std::unique_ptr<asio::ip::tcp::socket> connectTo(asio::io_context& ioc, std::uint16_t port) {
    auto socket = std::make_unique<asio::ip::tcp::socket>(ioc);
    socket->connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
    return socket;
}

enum class RemoteState { Open, Closed };

// Đọc hết dữ liệu có sẵn (bỏ qua nội dung) rồi cho biết bên kia đã đóng kết nối chưa, chờ tối đa `wait`.
RemoteState probeRemote(asio::ip::tcp::socket& socket, std::chrono::milliseconds wait) {
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::array<char, 512> buf{};
    while (std::chrono::steady_clock::now() < deadline) {
        asio::error_code ec;
        const std::size_t n = socket.read_some(asio::buffer(buf), ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        if (ec) {
            return RemoteState::Closed;  // EOF hoặc reset
        }
        (void)n;  // có dữ liệu (vd. handshake của session): tiếp tục đọc
    }
    return RemoteState::Open;
}

TorrentSessionConfig inboundTestConfig(const std::filesystem::path& dir, std::uint16_t port) {
    TorrentSessionConfig config;
    config.download_dir = dir;
    config.listen_port = port;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.enable_port_mapping = false;
    config.enable_utp = false;
    return config;
}

}  // namespace

void testInboundConnectionsAreLimitedPerIp() {
    std::cout << "[TEST] Inbound TCP connections are limited per IP..." << std::endl;
    const std::filesystem::path temp_dir = "./test_inbound_limit";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("inbound.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    auto config = inboundTestConfig(temp_dir, 7210);
    config.max_inbound_per_ip = 4;
    TorrentSession session(meta, config);
    CHECK(session.start());

    asio::io_context ioc;
    std::vector<std::unique_ptr<asio::ip::tcp::socket>> sockets;
    for (int i = 0; i < 10; ++i) {  // 10 kết nối cùng IP 127.0.0.1, im lặng
        sockets.push_back(connectTo(ioc, 7210));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    int closed = 0;
    int open = 0;
    for (auto& socket : sockets) {
        (probeRemote(*socket, std::chrono::milliseconds(300)) == RemoteState::Closed ? closed : open)++;
    }
    CHECK_EQ(open, 4);    // chỉ 4 kết nối đầu được giữ
    CHECK_EQ(closed, 6);  // 6 kết nối còn lại bị đóng ngay, chừa chỗ cho peer thật

    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testSilentInboundConnectionIsDroppedAfterHandshakeTimeout() {
    std::cout << "[TEST] Silent inbound connection is dropped after the handshake timeout, real peers survive..." << std::endl;
    const std::filesystem::path temp_dir = "./test_inbound_slowloris";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("slowloris.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    auto config = inboundTestConfig(temp_dir, 7211);
    config.handshake_timeout = std::chrono::seconds(1);
    config.peer_timeout = std::chrono::seconds(30);
    TorrentSession session(meta, config);
    CHECK(session.start());

    asio::io_context ioc;
    auto silent = connectTo(ioc, 7211);   // mở rồi im lặng: kiểu slowloris
    auto honest = connectTo(ioc, 7211);   // bắt tay đúng
    std::string handshake;
    handshake.push_back(static_cast<char>(19));
    handshake += "BitTorrent protocol";
    handshake += std::string(8, '\0');
    const std::string hex = session.info_hash_hex();
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        handshake.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    handshake += "-TEST00-abcdefghijkl";  // peer_id đủ 20 byte
    asio::write(*honest, asio::buffer(handshake));

    CHECK(probeRemote(*silent, std::chrono::milliseconds(3500)) == RemoteState::Closed);  // quá hạn bắt tay 1 giây
    CHECK(probeRemote(*honest, std::chrono::milliseconds(300)) == RemoteState::Open);     // đã bắt tay: không bị cắt

    // Chỗ do kết nối im lặng giữ đã được trả lại: kết nối mới vẫn được nhận.
    auto another = connectTo(ioc, 7211);
    CHECK(probeRemote(*another, std::chrono::milliseconds(300)) == RemoteState::Open);

    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testInboundCannotTakeSlotsReservedForOutbound() {
    std::cout << "[TEST] Inbound connections leave slots for outbound ones..." << std::endl;
    const std::filesystem::path temp_dir = "./test_inbound_reserve";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    auto meta = makeMockMetadata("reserve.bin", {"AlphaPiece123456", "BetaPiece1234567"}, 16);
    auto config = inboundTestConfig(temp_dir, 7212);
    config.max_connections = 10;     // 20% = 2 chỗ chừa cho kết nối ra
    config.max_inbound_per_ip = 0;   // tắt hạn mức theo IP để chỉ đo phần chừa chỗ
    TorrentSession session(meta, config);
    CHECK(session.start());

    asio::io_context ioc;
    std::vector<std::unique_ptr<asio::ip::tcp::socket>> sockets;
    for (int i = 0; i < 10; ++i) {
        sockets.push_back(connectTo(ioc, 7212));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    int open = 0;
    for (auto& socket : sockets) {
        open += probeRemote(*socket, std::chrono::milliseconds(250)) == RemoteState::Open ? 1 : 0;
    }
    CHECK_EQ(open, 8);  // 10 chỗ - 2 chừa cho kết nối ra

    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

void testStatsStaysResponsiveWhenAPeerStopsReading() {
    std::cout << "[TEST] stats() stays responsive while a peer stops reading its socket..." << std::endl;
    const std::filesystem::path temp_dir = "./test_stalled_peer";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    // 8 piece x 1 MiB để seed: đủ lớn để làm đầy bộ đệm gửi của socket tới peer không đọc.
    std::vector<std::string> pieces;
    for (int i = 0; i < 8; ++i) {
        pieces.push_back(std::string(1024 * 1024, static_cast<char>('a' + i)));
    }
    auto meta = makeMockMetadata("stalled_seed.bin", pieces, 1024 * 1024);
    {
        std::ofstream out(temp_dir / meta.name, std::ios::binary);
        for (const auto& piece : pieces) out << piece;
    }

    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7220;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = false;
    config.enable_port_mapping = false;
    config.enable_utp = false;
    config.choker_interval = std::chrono::seconds(1);  // choker gửi choke/unchoke mỗi giây
    TorrentSession session(meta, config);
    CHECK(session.start());
    CHECK(session.is_complete());

    // Peer giả: bắt tay, quan tâm, xin hàng trăm block rồi KHÔNG BAO GIỜ đọc nữa.
    asio::io_context ioc;
    asio::ip::tcp::socket stalled(ioc);
    stalled.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 7220));
    std::string handshake;
    handshake.push_back(static_cast<char>(19));
    handshake += "BitTorrent protocol";
    handshake += std::string(8, '\0');
    const std::string hex = session.info_hash_hex();
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        handshake.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    handshake += "-STALL0-abcdefghijk";
    asio::write(stalled, asio::buffer(handshake));

    auto be32 = [](std::string& out, std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
    };
    std::string messages;
    be32(messages, 1);
    messages.push_back(static_cast<char>(2));  // interested
    for (std::uint32_t piece = 0; piece < 8; ++piece) {
        for (std::uint32_t begin = 0; begin < 1024 * 1024; begin += 16384) {
            be32(messages, 13);
            messages.push_back(static_cast<char>(6));  // request
            be32(messages, piece);
            be32(messages, begin);
            be32(messages, 16384);
        }
    }
    asio::write(stalled, asio::buffer(messages));

    // Chờ để bộ đệm gửi đầy và choker chạy vài vòng, rồi đo: stats() (thứ Web UI gọi) không được chờ socket của peer.
    std::this_thread::sleep_for(std::chrono::seconds(4));
    long long worst_ms = 0;
    for (int i = 0; i < 30; ++i) {
        const auto start = std::chrono::steady_clock::now();
        (void)session.stats();
        worst_ms = std::max<long long>(worst_ms, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "  worst stats() latency: " << worst_ms << " ms" << std::endl;
    CHECK(worst_ms < 500);  // trước đây có thể treo tới io_timeout (30 giây)

    asio::error_code ignored;
    stalled.close(ignored);
    session.stop();
    std::filesystem::remove_all(temp_dir, ec);
}

// Đổi mạng khi session đang chạy: phải announce lại tracker NGAY (không chờ chu kỳ), và bỏ peer LAN của mạng cũ khỏi
// danh sách đã biết (nếu không, add_peer tưởng vẫn còn "peer LAN thay thế" và từ chối mọi peer khác).
void testNetworkChangeReannouncesAndDropsStaleLanPeers() {
    std::cout << "[TEST] network change re-announces to the tracker and forgets stale LAN peers..." << std::endl;
    const std::filesystem::path temp_dir = "./test_network_change";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);

    asio::io_context tracker_ioc;
    asio::ip::tcp::acceptor tracker_acceptor(
        tracker_ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const std::uint16_t tracker_port = tracker_acceptor.local_endpoint().port();
    std::atomic<int> announce_count{0};
    std::atomic<bool> stop_tracker{false};
    std::thread tracker_thread([&]() {
        while (!stop_tracker) {
            try {
                asio::ip::tcp::socket socket(tracker_ioc);
                asio::error_code accept_ec;
                tracker_acceptor.non_blocking(true);
                tracker_acceptor.accept(socket, accept_ec);
                if (accept_ec) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                socket.non_blocking(false);
                asio::streambuf request;
                asio::read_until(socket, request, "\r\n\r\n");
                ++announce_count;
                const std::string body = TrackerProtocol::encodeSuccessResponse(1800, {});
                const std::string http = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
                asio::write(socket, asio::buffer(http));
            } catch (...) {
            }
        }
    });

    // Mạng giả: ban đầu LAN 192.168.77.x, sau đó chuyển sang LAN khác 10.55.0.x.
    std::mutex net_mutex;
    network::NetworkSnapshot fake_net;
    fake_net.primary_ip = "192.168.77.5";
    fake_net.addresses = {"192.168.77.5"};
    network::NetworkMonitor::instance().configure_for_testing(
        [&]() {
            std::lock_guard<std::mutex> lock(net_mutex);
            return fake_net;
        },
        std::chrono::milliseconds(20), 1);

    auto meta = makeMockMetadata("netchange.bin", {"PieceZeroData!", "PieceOneData!!"}, 14);
    meta.announce = "http://127.0.0.1:" + std::to_string(tracker_port) + "/announce";
    TorrentSessionConfig config;
    config.download_dir = temp_dir;
    config.listen_port = 7261;
    config.enable_lsd = false;
    config.enable_dht = false;
    config.enable_tracker = true;
    config.enable_port_mapping = false;
    config.enable_utp = false;
    config.max_connections = 0;  // không quay số: ứng viên nằm yên để kiểm tra danh sách peer đã biết
    TorrentSession session(meta, config);
    CHECK(session.start());

    const auto wait_until = [](const std::function<bool()>& condition, int max_ms) {
        for (int waited = 0; waited < max_ms; waited += 20) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return condition();
    };
    CHECK(wait_until([&]() { return announce_count.load() >= 1; }, 5000));  // announce "started"

    session.add_peer("192.168.77.9", 7300);  // peer LAN của mạng cũ
    CHECK_EQ(session.stats().total_peers_known, std::size_t(1));

    // Chờ đủ lâu để chắc chắn announce kế tiếp (nếu có) không phải do chu kỳ/“đói peer” (cần >= 5 giây) mà do đổi mạng.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int before_change = announce_count.load();
    {
        std::lock_guard<std::mutex> lock(net_mutex);
        fake_net.primary_ip = "10.55.0.9";
        fake_net.addresses = {"10.55.0.9"};
    }
    const auto changed_at = std::chrono::steady_clock::now();
    CHECK(wait_until([&]() { return announce_count.load() > before_change; }, 4000));
    CHECK(std::chrono::steady_clock::now() - changed_at < std::chrono::seconds(4));
    CHECK(wait_until([&]() { return session.stats().total_peers_known == 0; }, 2000));

    // Sau đổi mạng, peer LAN mới được nhận bình thường (bộ lọc dùng IP mới, không dùng IP chốt cũ).
    session.add_peer("10.55.0.20", 7301);
    CHECK_EQ(session.stats().total_peers_known, std::size_t(1));

    session.stop();
    network::NetworkMonitor::instance().configure_for_testing(nullptr, std::chrono::seconds(3), 2);
    stop_tracker = true;
    tracker_thread.join();
    std::filesystem::remove_all(temp_dir, ec);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "fast") {  // chạy riêng nhóm Fast Extension khi gỡ lỗi
        testAllowedFastSetMatchesBep6Vectors();
        testFastExtensionSeederSendsHaveAllAllowedFastAndRejects();
        testFastExtensionLeecherDownloadsAllowedPieceWhileChoked();
        return TEST_REPORT();
    }
    testMagnetResolverFetchesAndVerifiesMetadata();
    testMagnetResolverRetriesUntilTrackerHasPeers();
    testMagnetResolverFailsFastWithoutAnySource();
    testMagnetFetchStopsWhenAttemptBudgetIsExhausted();
    testTorrentSessionServesBep9Metadata();
    testTorrentSessionConnectsToPexPeer();
    testSessionResumesFromPartFile();
    testSessionInitializationAndStats();
    testSessionTrackerDisabledStillRunsDht();
    testSessionDhtIntegration();
    testSessionStartDoesNotBlockOnDhtBootstrap();
    testSessionExistingFileSeeding();
    testSessionPartialExistingFileRemainsDownloading();
    testSessionFailsWhenListenPortIsAlreadyBusy();
    testStateBridgeKeepsOriginalMagnetForDuplicateTorrent();
    testSessionLoopbackPeerDownload();
    testSessionLoopbackDownloadWithUnreachableTracker();
    testSessionRateLimitsAndPauseResume();
    testStateBridgePersistsAndRestoresTorrents();
    testStateBridgeSanitizesMagnetAndPersistsDiscoveryFlags();
    testStateBridgePickerStrategyPerTorrentAndForAll();
    testDownloadQueueLimitsActiveTorrents();
    testGlobalBandwidthLimitIsSharedAndPersisted();
    testNetworkChangeReannouncesAndDropsStaleLanPeers();
    testAllowedFastSetMatchesBep6Vectors();
    testFastExtensionSeederSendsHaveAllAllowedFastAndRejects();
    testFastExtensionLeecherDownloadsAllowedPieceWhileChoked();
    testSessionMultiFileExistingSeeding();
    testSessionMultiFilePartialDataResumes();
    testSessionMultiFileLoopbackDownload();
    testFileSelectionMapsToPiecesAndValidatesInput();
    testFileSelectionWithExistingDataChangesSeedingState();
    testFileSelectionLoopbackDownloadsOnlySelectedPieces();
    testInboundConnectionsAreLimitedPerIp();
    testSilentInboundConnectionIsDroppedAfterHandshakeTimeout();
    testInboundCannotTakeSlotsReservedForOutbound();
    testStatsStaysResponsiveWhenAPeerStopsReading();

    return TEST_REPORT();
}

