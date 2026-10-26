#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/torrent_builder.h"
#include "network/peer_connection.h"
#include "network/peer_protocol.h"
#include "network/torrent_session.h"
#include "webui/state_bridge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

            // 3. Nhận interested từ TorrentSession
            auto interested_msg = seeder_peer.receive_message();
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

            // 3. Nhận interested từ TorrentSession
            auto interested_msg = seeder_peer.receive_message();
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

int main() {
    testSessionResumesFromPartFile();
    testSessionInitializationAndStats();
    testSessionExistingFileSeeding();
    testSessionPartialExistingFileRemainsDownloading();
    testSessionFailsWhenListenPortIsAlreadyBusy();
    testStateBridgeKeepsOriginalMagnetForDuplicateTorrent();
    testSessionLoopbackPeerDownload();
    testSessionLoopbackDownloadWithUnreachableTracker();

    return TEST_REPORT();
}

