// Test tích hợp WebUI qua HTTP thật: server httplib chạy trên loopback, gọi bằng httplib::Client.
// Mọi file ghi ra nằm trong thư mục tạm (cwd được chuyển sang đó TRƯỚC khi registerRoutes), không đụng data/ của repo.
#include "test_framework.h"

#include "httplib.h"

#include "core/torrent_file.h"
#include "network/lan_address.h"
#include "webui/api_routes.h"
#include "webui/http_server.h"
#include "webui/state_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define P2P_GETPID _getpid
#else
#include <unistd.h>
#define P2P_GETPID getpid
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kPort = 18081;

fs::path g_workDir;       // cwd trong suốt test
fs::path g_downloadDir;   // g_workDir/data/downloads

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

httplib::Client makeClient() {
    httplib::Client client("127.0.0.1", kPort);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(120, 0);
    return client;
}

httplib::UploadFormDataItems createForm(const std::string& filename, const std::string& content,
                                        const std::vector<std::pair<std::string, std::string>>& fields = {}) {
    httplib::UploadFormDataItems items;
    items.push_back({"file", content, filename, "application/octet-stream"});
    // Tắt tracker/DHT: test không được chạm ra ngoài loopback.
    items.push_back({"enable_tracker", "0", "", ""});
    items.push_back({"enable_dht", "0", "", ""});
    items.push_back({"announce", "http://127.0.0.1:1/announce", "", ""});
    for (const auto& [name, value] : fields) {
        items.push_back({name, value, "", ""});
    }
    return items;
}

bool hasHeader(const httplib::Response& res, const char* name) {
    return res.has_header(name);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

// ---- Các ca kiểm thử ----

void testSecurityHeadersAndLoopbackBinding() {
    auto client = makeClient();
    auto res = client.Get("/");
    CHECK(res != nullptr);
    if (!res) return;
    CHECK_EQ(res->status, 200);
    CHECK_EQ(res->get_header_value("X-Content-Type-Options"), std::string("nosniff"));
    CHECK_EQ(res->get_header_value("X-Frame-Options"), std::string("DENY"));
    CHECK_EQ(res->get_header_value("Referrer-Policy"), std::string("no-referrer"));

    // Header an toàn cũng có trên phản hồi API và lỗi.
    auto api = client.Get("/api/status");
    CHECK(api != nullptr);
    if (api) CHECK_EQ(api->get_header_value("X-Content-Type-Options"), std::string("nosniff"));
    auto missing = client.Get("/api/does-not-exist");
    CHECK(missing != nullptr);
    if (missing) CHECK_EQ(missing->get_header_value("X-Frame-Options"), std::string("DENY"));

    // Mặc định chỉ nghe loopback: IP LAN của máy này không kết nối được tới WebUI.
    const std::string lanIp = p2p::network::detect_lan_ip();
    if (!lanIp.empty() && lanIp != "127.0.0.1" && lanIp != "0.0.0.0") {
        httplib::Client lan(lanIp, kPort);
        lan.set_connection_timeout(3, 0);
        lan.set_read_timeout(3, 0);
        auto viaLan = lan.Get("/");
        CHECK(viaLan == nullptr);
    } else {
        std::cout << "  (bo qua kiem tra IP LAN: khong co dia chi LAN)" << std::endl;
    }
}

void testFileContentServesOnlySafeTypesInline() {
    writeFile(g_downloadDir / "evil.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>");
    writeFile(g_downloadDir / "pic.png", "\x89PNG\r\n\x1a\nfake");
    writeFile(g_downloadDir / "page.html", "<script>alert(1)</script>");
    writeFile(g_downloadDir / "note.txt", "hello");

    auto client = makeClient();
    auto svg = client.Get("/api/files/content?name=evil.svg");
    CHECK(svg != nullptr);
    if (svg) {
        CHECK_EQ(svg->status, 200);
        CHECK(startsWith(svg->get_header_value("Content-Disposition"), "attachment"));
        CHECK_EQ(svg->get_header_value("Content-Type"), std::string("application/octet-stream"));
        CHECK_EQ(svg->get_header_value("X-Content-Type-Options"), std::string("nosniff"));
        CHECK(!hasHeader(*svg, "Access-Control-Allow-Origin"));
        CHECK(svg->get_header_value("Content-Security-Policy").find("sandbox") != std::string::npos);
    }

    auto html = client.Get("/api/files/content?path=page.html");
    CHECK(html != nullptr);
    if (html) {
        CHECK(startsWith(html->get_header_value("Content-Disposition"), "attachment"));
        CHECK_EQ(html->get_header_value("Content-Type"), std::string("application/octet-stream"));
    }

    auto png = client.Get("/api/files/content?name=pic.png");
    CHECK(png != nullptr);
    if (png) {
        CHECK_EQ(png->status, 200);
        CHECK(startsWith(png->get_header_value("Content-Disposition"), "inline"));
        CHECK_EQ(png->get_header_value("Content-Type"), std::string("image/png"));
        CHECK_EQ(png->get_header_value("X-Content-Type-Options"), std::string("nosniff"));
        CHECK(!hasHeader(*png, "Access-Control-Allow-Origin"));
    }

    auto txt = client.Get("/api/files/content?name=note.txt");
    CHECK(txt != nullptr);
    if (txt) {
        CHECK(startsWith(txt->get_header_value("Content-Disposition"), "inline"));
        CHECK_EQ(txt->body, std::string("hello"));
    }
}

void testFileContentNameBranchBlocksAlternateStreamsAndHugeFiles() {
    writeFile(g_downloadDir / "a.txt", "plain");
    // Luồng dữ liệu phụ NTFS "a.txt:stream" (chỉ có trên Windows/NTFS).
    bool streamCreated = false;
    {
        std::ofstream stream(g_downloadDir / "a.txt:stream", std::ios::binary);
        if (stream) {
            stream << "secret-stream";
            streamCreated = true;
        }
    }
    auto client = makeClient();
    auto res = client.Get("/api/files/content?name=a.txt%3Astream");
    CHECK(res != nullptr);
    if (res) {
        CHECK_EQ(res->status, 404);
        CHECK(res->body.find("secret-stream") == std::string::npos);
    }
    if (!streamCreated) {
        std::cout << "  (khong tao duoc alternate data stream: ca nay chi kiem tra 404)" << std::endl;
    }

    // File lớn hơn 128 MiB: 413 + JSON, không cấp phát cả file.
    const fs::path big = g_downloadDir / "big.png";
    {
        std::ofstream create(big, std::ios::binary);
    }
    std::error_code ec;
    fs::resize_file(big, 129ull * 1024 * 1024, ec);
    CHECK(!ec);
    auto tooBig = client.Get("/api/files/content?name=big.png");
    CHECK(tooBig != nullptr);
    if (tooBig) {
        CHECK_EQ(tooBig->status, 413);
        CHECK(tooBig->body.find("File too large to preview") != std::string::npos);
        CHECK(tooBig->body.size() < 1024);
    }
    fs::remove(big, ec);
}

void testCreateTorrentClampsPieceLength() {
    auto client = makeClient();
    auto res = client.Post("/api/torrents/create",
                           createForm("piece_clamp.bin", std::string(4096, 'p'), {{"piece_length", "1"}}));
    CHECK(res != nullptr);
    if (!res) return;
    CHECK_EQ(res->status, 200);
    if (res->status != 200) return;
    const auto meta = p2p::TorrentFile::parse(res->body);
    CHECK(meta.pieceLength >= 16384);
    CHECK_EQ(meta.pieceLength, std::int64_t(262144));

    // Lũy thừa của 2 hợp lệ được giữ nguyên; giá trị không phải lũy thừa của 2 thì về mặc định.
    auto okRes = client.Post("/api/torrents/create",
                             createForm("piece_ok.bin", std::string(40000, 'q'), {{"piece_length", "32768"}}));
    CHECK(okRes != nullptr);
    if (okRes && okRes->status == 200) {
        CHECK_EQ(p2p::TorrentFile::parse(okRes->body).pieceLength, std::int64_t(32768));
    }
    auto oddRes = client.Post("/api/torrents/create",
                              createForm("piece_odd.bin", std::string(40000, 'r'), {{"piece_length", "20000"}}));
    CHECK(oddRes != nullptr);
    if (oddRes && oddRes->status == 200) {
        CHECK_EQ(p2p::TorrentFile::parse(oddRes->body).pieceLength, std::int64_t(262144));
    }
}

void testCreateTorrentDoesNotOverwriteExistingFile() {
    auto client = makeClient();
    auto first = client.Post("/api/torrents/create", createForm("dup.txt", "first-content-aaaa"));
    CHECK(first != nullptr);
    if (!first) return;
    CHECK_EQ(first->status, 200);
    auto second = client.Post("/api/torrents/create", createForm("dup.txt", "second-content-bbbb"));
    CHECK(second != nullptr);
    if (!second) return;
    CHECK_EQ(second->status, 200);

    CHECK_EQ(readFile(g_downloadDir / "dup.txt"), std::string("first-content-aaaa"));
    CHECK(fs::exists(g_downloadDir / "dup (1).txt"));
    CHECK_EQ(readFile(g_downloadDir / "dup (1).txt"), std::string("second-content-bbbb"));
    CHECK(second->get_header_value("Content-Disposition").find("dup (1).txt.torrent") != std::string::npos);
    CHECK_EQ(p2p::TorrentFile::parse(second->body).name, std::string("dup (1).txt"));
}

void testCreateTorrentValidatesBeforeWritingFile() {
    auto client = makeClient();
    auto res = client.Post("/api/torrents/create",
                           createForm("orphan.txt", "should-not-be-stored", {{"name", "../evil"}}));
    CHECK(res != nullptr);
    if (res) CHECK_EQ(res->status, 400);
    CHECK(!fs::exists(g_downloadDir / "orphan.txt"));
}

void testAddMagnetRejectsControlCharacters() {
    const std::string hash(40, 'a');
    httplib::Client client("127.0.0.1", kPort);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);  // bản cũ đi vào resolve và treo: để test thất bại nhanh thay vì chờ
    for (const std::string tail : {std::string("\npaused=1"), std::string("\r\ndown=1"), std::string("\x01")}) {
        httplib::UploadFormDataItems items;
        items.push_back({"magnet_uri", "magnet:?xt=urn:btih:" + hash + "&dn=x" + tail, "", ""});
        items.push_back({"enable_tracker", "0", "", ""});
        items.push_back({"enable_dht", "0", "", ""});
        auto res = client.Post("/api/torrents/add-magnet", items);
        CHECK(res != nullptr);
        if (res) {
            CHECK_EQ(res->status, 400);
            CHECK(res->body.find("control characters") != std::string::npos);
        }
    }
}

void testLimitIsAtomicWhenOneValueIsInvalid() {
    auto client = makeClient();
    auto created = client.Post("/api/torrents/create", createForm("limit_me.bin", std::string(2048, 'l')));
    CHECK(created != nullptr);
    if (!created || created->status != 200) return;
    const std::string magnet = created->get_header_value("X-Magnet-Uri");
    const std::string marker = "urn:btih:";
    const auto pos = magnet.find(marker);
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    const std::string hash = magnet.substr(pos + marker.size(), 40);

    auto session = p2p::StateBridge::instance().getSession(hash);
    CHECK(session != nullptr);
    if (!session) return;

    auto ok = client.Post("/api/torrents/limit?hash=" + hash + "&down=100&up=50", "", "text/plain");
    CHECK(ok != nullptr);
    if (ok) CHECK_EQ(ok->status, 200);
    CHECK_EQ(session->download_limit(), std::uint64_t(100) * 1024);
    CHECK_EQ(session->upload_limit(), std::uint64_t(50) * 1024);

    // down hợp lệ + up sai: 400 và KHÔNG giá trị nào bị đổi.
    auto bad = client.Post("/api/torrents/limit?hash=" + hash + "&down=200&up=abc", "", "text/plain");
    CHECK(bad != nullptr);
    if (bad) CHECK_EQ(bad->status, 400);
    CHECK_EQ(session->download_limit(), std::uint64_t(100) * 1024);
    CHECK_EQ(session->upload_limit(), std::uint64_t(50) * 1024);
}

// Chạy cuối vì phải chờ resolve hết hạn (~45 giây) trước khi server dừng được.
void testMagnetLookupsAreLimited() {
    std::atomic<int> tooMany{0};
    std::atomic<int> other{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([i, &tooMany, &other]() {
            auto client = makeClient();
            // Tracker từ chối kết nối nên resolve thử lại tới hết hạn, giữ 1 luồng worker của server.
            const std::string hash = std::string(39, 'b') + static_cast<char>('0' + i);
            httplib::UploadFormDataItems items;
            items.push_back({"magnet_uri", "magnet:?xt=urn:btih:" + hash + "&tr=http%3A%2F%2F127.0.0.1%3A1%2Fannounce",
                             "", ""});
            items.push_back({"enable_dht", "0", "", ""});
            auto res = client.Post("/api/torrents/add-magnet", items);
            if (res && res->status == 429) {
                ++tooMany;
            } else {
                ++other;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // đảm bảo thứ tự: 3 request đầu giữ slot
    }
    for (auto& worker : workers) worker.join();
    CHECK_EQ(tooMany.load(), 1);
    CHECK_EQ(other.load(), 3);
}

}  // namespace

int main() {
    // Không để biến môi trường của người chạy mở LAN hay đổi bootstrap DHT.
#ifdef _WIN32
    _putenv("P2P_WEBUI_ALLOW_REMOTE=");
    _putenv("DHT_BOOTSTRAP_HOST=");
    _putenv("DHT_BOOTSTRAP_PORT=");
#else
    unsetenv("P2P_WEBUI_ALLOW_REMOTE");
    unsetenv("DHT_BOOTSTRAP_HOST");
    unsetenv("DHT_BOOTSTRAP_PORT");
#endif

    std::error_code ec;
    const fs::path originalCwd = fs::current_path();
    const fs::path tempRoot = fs::temp_directory_path() / ("p2p_webui_api_test_" + std::to_string(P2P_GETPID()));
    fs::remove_all(tempRoot, ec);
    g_workDir = tempRoot / "work";
    const fs::path siteDir = tempRoot / "site";
    fs::create_directories(g_workDir);
    writeFile(siteDir / "index.html", "<html><body>test</body></html>");

    // Chuyển cwd sang thư mục tạm TRƯỚC registerRoutes để resolveProjectRoot() không trỏ vào repo thật.
    fs::current_path(g_workDir);
    g_workDir = fs::current_path();
    g_downloadDir = g_workDir / "data" / "downloads";

    bool isolated = true;
    for (fs::path dir = g_workDir; ; dir = dir.parent_path()) {
        if (fs::exists(dir / "CMakeLists.txt") || (fs::exists(dir / "src") && fs::exists(dir / "web"))) {
            isolated = false;
        }
        if (dir == dir.parent_path()) break;
    }
    CHECK(isolated);
    if (!isolated) {
        std::cerr << "Thu muc tam nam trong mot cay du an; dung de tranh ghi vao du lieu that." << std::endl;
        fs::current_path(originalCwd);
        fs::remove_all(tempRoot, ec);
        return TEST_REPORT();
    }

    {
        p2p::HttpServer server(siteDir);
        p2p::ApiRoutes::registerRoutes(server.raw(), g_workDir / "uploads");
        std::thread serverThread([&server]() { server.run(kPort); });
        server.raw().wait_until_ready();

        RUN(testSecurityHeadersAndLoopbackBinding);
        RUN(testFileContentServesOnlySafeTypesInline);
        RUN(testFileContentNameBranchBlocksAlternateStreamsAndHugeFiles);
        RUN(testCreateTorrentClampsPieceLength);
        RUN(testCreateTorrentDoesNotOverwriteExistingFile);
        RUN(testCreateTorrentValidatesBeforeWritingFile);
        RUN(testAddMagnetRejectsControlCharacters);
        RUN(testLimitIsAtomicWhenOneValueIsInvalid);
        RUN(testMagnetLookupsAreLimited);

        server.raw().stop();
        serverThread.join();
    }

    // Dừng mọi session và trả StateBridge về trạng thái không lưu, rồi dọn thư mục tạm.
    auto& bridge = p2p::StateBridge::instance();
    for (const auto& torrent : bridge.getAllTorrents()) {
        bridge.removeSession(torrent.info_hash_hex);
    }
    bridge.setStateDir({});
    fs::current_path(originalCwd);
    fs::remove_all(tempRoot, ec);
    const int result = TEST_REPORT();
    std::cout.flush();
    std::_Exit(result);  // các luồng nền (DHT, hàng đợi, khôi phục) không cần dừng êm khi test kết thúc
}
