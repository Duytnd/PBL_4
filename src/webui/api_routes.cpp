#include "webui/api_routes.h"

#include "httplib.h"

#include "core/hash_utils.h"
#include "core/path_sanitizer.h"
#include "core/torrent_builder.h"
#include "core/torrent_file.h"
#include "network/lan_address.h"
#include "webui/request_guard.h"
#include "network/magnet_resolver.h"
#include "network/tracker_client.h"
#include "webui/state_bridge.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <spawn.h>
#include <sys/wait.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace p2p {
namespace {

std::filesystem::path resolveProjectRoot() {
    std::filesystem::path current = std::filesystem::absolute(std::filesystem::current_path());
    for (int depth = 0; depth < 8; ++depth) {
        const bool looksLikeProjectRoot =
            std::filesystem::exists(current / "CMakeLists.txt") &&
            std::filesystem::exists(current / "src") &&
            std::filesystem::exists(current / "web");
        if (looksLikeProjectRoot) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::filesystem::absolute(std::filesystem::current_path());
}

std::filesystem::path resolveDownloadDir() {
    return resolveProjectRoot() / "data" / "downloads";
}

std::string getLocalLanIp() {
    return network::detect_lan_ip();
}

// Tracker mặc định nằm trên chính máy này: dùng IP LAN, không dùng 127.0.0.1 (máy khác sẽ announce
// vào loopback của chính nó và không bao giờ thấy peer nào).
std::string defaultAnnounceUrl() {
    return "http://" + getLocalLanIp() + ":6969/announce";
}

constexpr std::size_t kDefaultPieceLength = 262144;
constexpr std::size_t kMaxUploadBytes = 256ull * 1024 * 1024;

std::string rfc5987Encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string asciiFallbackName(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool printableAscii = c >= 0x20 && c <= 0x7E && c != '"' && c != '\\';
        out.push_back(printableAscii ? static_cast<char>(c) : '_');
    }
    return out;
}

std::string buildContentDisposition(const std::string& downloadName) {
    return "attachment; filename=\"" + asciiFallbackName(downloadName) +
           "\"; filename*=UTF-8''" + rfc5987Encode(downloadName);
}

std::filesystem::path safeFileName(const std::string& rawName) {
    std::filesystem::path name = std::filesystem::path(rawName).filename();
    if (name.empty() || name == "." || name == "..") {
        name = "upload.bin";
    }
    return name;
}

std::size_t parsePieceLength(const httplib::MultipartFormData& form) {
    if (!form.has_field("piece_length")) {
        return kDefaultPieceLength;
    }
    try {
        const long long value = std::stoll(form.get_field("piece_length"));
        if (value <= 0) {
            return kDefaultPieceLength;
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        return kDefaultPieceLength;
    }
}

bool parseBoolField(const httplib::MultipartFormData& form, const char* name, bool defaultValue) {
    if (!form.has_field(name)) {
        return defaultValue;
    }
    const std::string value = form.get_field(name);
    if (value.empty()) {
        return defaultValue;
    }
    const std::string normalized = [&value]() {
        std::string copy = value;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return copy;
    }();
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return defaultValue;
}

std::uint16_t parseUint16Field(const httplib::MultipartFormData& form, const char* name, std::uint16_t defaultValue) {
    if (!form.has_field(name)) {
        return defaultValue;
    }
    try {
        const unsigned long value = std::stoul(form.get_field(name));
        if (value == 0 || value > 65535UL) {
            return defaultValue;
        }
        return static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
        return defaultValue;
    }
}

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string readEnvString(const char* key) {
    if (const char* value = std::getenv(key)) {
        return trim(value);
    }
    return "";
}

std::filesystem::path resolveDhtConfigPath() {
    return resolveProjectRoot() / "config" / "dht_bootstrap.json";
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string readJsonStringField(const std::string& json, const std::string& field) {
    const std::regex re("\\\"" + field + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    if (std::regex_search(json, match, re)) {
        return match[1].str();
    }
    return "";
}

std::uint16_t readJsonUint16Field(const std::string& json, const std::string& field, std::uint16_t defaultValue) {
    const std::regex re("\\\"" + field + "\\\"\\s*:\\s*([0-9]{1,5})");
    std::smatch match;
    if (std::regex_search(json, match, re)) {
        const unsigned long value = std::stoul(match[1].str());
        if (value > 0 && value <= 65535UL) {
            return static_cast<std::uint16_t>(value);
        }
    }
    return defaultValue;
}

std::pair<std::string, std::uint16_t> loadDhtBootstrapConfig() {
    std::string host = readEnvString("DHT_BOOTSTRAP_HOST");
    std::uint16_t port = 0;
    const std::string envPort = readEnvString("DHT_BOOTSTRAP_PORT");
    if (!envPort.empty()) {
        try {
            const unsigned long value = std::stoul(envPort);
            if (value > 0 && value <= 65535UL) {
                port = static_cast<std::uint16_t>(value);
            }
        } catch (const std::exception&) {
        }
    }

    const std::filesystem::path configPath = resolveDhtConfigPath();
    if (std::filesystem::exists(configPath)) {
        std::ifstream input(configPath);
        if (input) {
            std::stringstream buffer;
            buffer << input.rdbuf();
            const std::string json = buffer.str();
            if (host.empty()) {
                host = readJsonStringField(json, "host");
            }
            if (port == 0) {
                port = readJsonUint16Field(json, "port", 0);
            }
        }
    }

    return {host, port};
}

// KHÔNG đoán bootstrap DHT từ địa chỉ tracker nữa. Bản trước giả định có node DHT ở <máy tracker>:6881, nhưng cổng đó
// thường là chương trình BitTorrent khác (đã gặp btweb.exe trên máy thử), khiến node của dự án gia nhập DHT Internet
// thật và nhận về IP public cùng peer lạ. Bootstrap chỉ dùng cấu hình tường minh (ô nhập UI, biến môi trường, file
// config); mặc định để trống và peer vẫn tìm nhau qua LSD, tracker và thông điệp Port giữa các peer.
std::pair<std::string, std::uint16_t> withTrackerBootstrapFallback(
    std::string host, std::uint16_t port, const std::string& /*announceUrl*/) {
    if (!host.empty() && port == 0) {
        port = 6881;
    }
    return {host, port};
}

// Tracker đầu tiên trong magnet (tham số tr=), "" nếu không có hoặc magnet sai.
std::string magnetFirstTracker(const std::string& magnetUri) {
    try {
        const auto magnet = network::parse_magnet_uri(magnetUri);
        return magnet.trackers.empty() ? std::string() : magnet.trackers.front();
    } catch (const std::exception&) {
        return {};
    }
}

std::string getDhtBootstrapSource() {
    if (!readEnvString("DHT_BOOTSTRAP_HOST").empty() || !readEnvString("DHT_BOOTSTRAP_PORT").empty()) {
        return "env";
    }
    return std::filesystem::exists(resolveDhtConfigPath()) ? "json" : "default";
}

void saveDhtBootstrapConfig(const std::string& host, std::uint16_t port) {
    const std::filesystem::path configPath = resolveDhtConfigPath();
    std::filesystem::create_directories(configPath.parent_path());
    std::ofstream output(configPath, std::ios::trunc);
    if (!output) {
        return;
    }
    output << "{\n"
           << "  \"host\": \"" << jsonEscape(host) << "\",\n"
           << "  \"port\": " << port << "\n"
           << "}\n";

    std::string portValue = std::to_string(port);
#ifdef _WIN32
    _putenv_s("DHT_BOOTSTRAP_HOST", host.c_str());
    _putenv_s("DHT_BOOTSTRAP_PORT", portValue.c_str());
#else
    setenv("DHT_BOOTSTRAP_HOST", host.c_str(), 1);
    setenv("DHT_BOOTSTRAP_PORT", portValue.c_str(), 1);
#endif
}

std::pair<std::string, std::uint16_t> extractDhtBootstrapFromRequest(const httplib::Request& req) {
    std::string host;
    std::uint16_t port = 0;

    if (req.has_param("host")) {
        host = trim(req.get_param_value("host"));
    } else if (req.has_param("dht_bootstrap_host")) {
        host = trim(req.get_param_value("dht_bootstrap_host"));
    } else if (!req.body.empty()) {
        const std::regex hostRegex(R"re("host"\s*:\s*"([^"]*)")re");
        std::smatch match;
        if (std::regex_search(req.body, match, hostRegex)) {
            host = match[1].str();
        }
    }

    if (req.has_param("port")) {
        try {
            const unsigned long value = std::stoul(req.get_param_value("port"));
            if (value > 0 && value <= 65535UL) {
                port = static_cast<std::uint16_t>(value);
            }
        } catch (const std::exception&) {
        }
    } else if (req.has_param("dht_bootstrap_port")) {
        try {
            const unsigned long value = std::stoul(req.get_param_value("dht_bootstrap_port"));
            if (value > 0 && value <= 65535UL) {
                port = static_cast<std::uint16_t>(value);
            }
        } catch (const std::exception&) {
        }
    } else if (!req.body.empty()) {
        const std::regex portRegex(R"re("port"\s*:\s*([0-9]{1,5}))re");
        std::smatch match;
        if (std::regex_search(req.body, match, portRegex)) {
            const unsigned long value = std::stoul(match[1].str());
            if (value > 0 && value <= 65535UL) {
                port = static_cast<std::uint16_t>(value);
            }
        }
    }

    return {host, port};
}

void handleCreateTorrent(const httplib::Request& req, httplib::Response& res) {
    if (!req.form.has_file("file")) {
        res.status = 400;
        res.set_content("Missing \"file\" field", "text/plain");
        return;
    }

    const httplib::FormData upload = req.form.get_file("file");
    if (upload.content.size() > kMaxUploadBytes) {
        res.status = 413;
        res.set_content("File too large", "text/plain");
        return;
    }

    std::filesystem::path downloadDir = resolveDownloadDir();
    std::error_code ec;
    std::filesystem::create_directories(downloadDir, ec);

    const std::filesystem::path sourceName = safeFileName(upload.filename);
    const std::filesystem::path persistentPath = downloadDir / sourceName;

    {
        std::ofstream output(persistentPath, std::ios::binary);
        if (!output) {
            res.status = 500;
            res.set_content("Failed to store uploaded file", "text/plain");
            return;
        }
        output.write(upload.content.data(), static_cast<std::streamsize>(upload.content.size()));
    }

    const std::string announce = req.form.has_field("announce") && !req.form.get_field("announce").empty()
        ? req.form.get_field("announce")
        : defaultAnnounceUrl();
    const std::size_t pieceLength = parsePieceLength(req.form);
    const std::string nameOverride = req.form.has_field("name") ? req.form.get_field("name") : "";
    const bool enableTracker = parseBoolField(req.form, "enable_tracker", true);
    const bool enableDht = parseBoolField(req.form, "enable_dht", true);
    const auto [defaultDhtHost, defaultDhtPort] = loadDhtBootstrapConfig();
    const std::string rawDhtBootstrapHost = req.form.has_field("dht_bootstrap_host") ? req.form.get_field("dht_bootstrap_host") : defaultDhtHost;
    const std::uint16_t rawDhtBootstrapPort = req.form.has_field("dht_bootstrap_port") ? parseUint16Field(req.form, "dht_bootstrap_port", 0) : defaultDhtPort;
    const auto [dhtBootstrapHost, dhtBootstrapPort] =
        withTrackerBootstrapFallback(rawDhtBootstrapHost, rawDhtBootstrapPort, announce);
    if (!nameOverride.empty() && !PathSanitizer::isSafeRelativePath(nameOverride)) {
        res.status = 400;
        res.set_content("Invalid torrent name", "text/plain");
        return;
    }

    try {
        const std::string torrentBytes = TorrentBuilder::buildTorrent(
            persistentPath, announce, pieceLength, nameOverride);

        // Parse metadata để lấy info_hash và tạo Magnet URI
        auto meta = TorrentFile::parse(torrentBytes);
        std::string infoHashHex = sha1Hex(meta.infoRaw);
        std::string magnetUri = "magnet:?xt=urn:btih:" + infoHashHex +
                                "&dn=" + rfc5987Encode(meta.name) +
                                "&tr=" + rfc5987Encode(announce);

        // Khởi động TorrentSession chạy ngầm ở chế độ Seeder
        TorrentSessionConfig config;
        StateBridge::instance().applyGlobalLimiters(config);
        config.download_dir = downloadDir;
        config.listen_port = StateBridge::instance().allocatePort();
        config.enable_tracker = enableTracker;
        config.enable_lsd = true;
        config.enable_dht = enableDht;
        config.dht_bootstrap_host = dhtBootstrapHost;
        config.dht_bootstrap_port = dhtBootstrapPort;
        if (enableDht) {
            config.shared_dht = StateBridge::instance().getOrCreateSharedDht(
                6881, dhtBootstrapHost, dhtBootstrapPort);
        }

        // Trùng torrent: không start thêm session (tránh chiếm port vô ích).
        if (!StateBridge::instance().getSession(infoHashHex)) {
            auto session = std::make_shared<TorrentSession>(meta, config);
            if (!session->start()) {
                res.status = 503;
                res.set_content("Failed to start torrent session: port already in use or bind failed", "text/plain");
                return;
            }
            StateBridge::instance().addSession(session, magnetUri);
        }

        const std::string downloadName =
            (nameOverride.empty() ? sourceName.string() : nameOverride) + ".torrent";

        res.set_header("Content-Disposition", buildContentDisposition(downloadName));
        res.set_header("X-Magnet-Uri", magnetUri);
        res.set_header("Access-Control-Expose-Headers", "Content-Disposition, X-Magnet-Uri");
        res.set_content(torrentBytes, "application/x-bittorrent");
    } catch (const std::exception& error) {
        res.status = 500;
        res.set_content(std::string("Failed to build torrent: ") + error.what(), "text/plain");
    }
}

void handleAddTorrent(const httplib::Request& req, httplib::Response& res) {
    if (!req.form.has_file("file")) {
        res.status = 400;
        res.set_content("Missing \"file\" field", "text/plain");
        return;
    }

    const httplib::FormData upload = req.form.get_file("file");
    try {
        auto meta = TorrentFile::parse(upload.content);
        // Số piece phải khớp độ dài file, nếu không PieceManager đọc/ghi sai vùng nhớ.
        const std::uint64_t pieceLen = static_cast<std::uint64_t>(meta.pieceLength);
        if (meta.length == 0 || meta.pieces.size() != (meta.length + pieceLen - 1) / pieceLen) {
            res.status = 400;
            res.set_content("Invalid torrent: piece count does not match file length", "text/plain");
            return;
        }
        std::string infoHashHex = sha1Hex(meta.infoRaw);
        std::string magnetUri = "magnet:?xt=urn:btih:" + infoHashHex +
                                "&dn=" + rfc5987Encode(meta.name) +
                                "&tr=" + rfc5987Encode(meta.announce);

        std::filesystem::path downloadDir = resolveDownloadDir();
        std::error_code ec;
        std::filesystem::create_directories(downloadDir, ec);

        TorrentSessionConfig config;
        StateBridge::instance().applyGlobalLimiters(config);
        config.download_dir = downloadDir;
        config.listen_port = StateBridge::instance().allocatePort();
        config.enable_tracker = parseBoolField(req.form, "enable_tracker", true);
        config.enable_lsd = true;
        config.enable_dht = parseBoolField(req.form, "enable_dht", true);
        const auto [defaultDhtHost, defaultDhtPort] = loadDhtBootstrapConfig();
        std::tie(config.dht_bootstrap_host, config.dht_bootstrap_port) = withTrackerBootstrapFallback(
            req.form.has_field("dht_bootstrap_host") ? req.form.get_field("dht_bootstrap_host") : defaultDhtHost,
            req.form.has_field("dht_bootstrap_port") ? parseUint16Field(req.form, "dht_bootstrap_port", 0) : defaultDhtPort,
            meta.announce);
        if (config.enable_dht) {
            config.shared_dht = StateBridge::instance().getOrCreateSharedDht(
                6881, config.dht_bootstrap_host, config.dht_bootstrap_port);
        }

        if (!StateBridge::instance().getSession(infoHashHex)) {
            auto session = std::make_shared<TorrentSession>(meta, config);
            if (!session->start()) {
                res.status = 503;
                res.set_content("Failed to start torrent session: port already in use or bind failed", "text/plain");
                return;
            }
            StateBridge::instance().addSession(session, magnetUri);
        }

        res.status = 200;
        res.set_content("{\"status\":\"ok\",\"info_hash\":\"" + infoHashHex + "\"}", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("Failed to parse torrent: ") + e.what(), "text/plain");
    }
}

void handleAddMagnet(const httplib::Request& req, httplib::Response& res) {
    if (!req.form.has_field("magnet_uri")) {
        res.status = 400;
        res.set_content("Missing magnet_uri field", "text/plain");
        return;
    }

    const std::string magnetUri = req.form.get_field("magnet_uri");
    try {
        const auto magnet = network::parse_magnet_uri(magnetUri);
        const std::filesystem::path downloadDir = resolveDownloadDir();
        std::error_code ec;
        std::filesystem::create_directories(downloadDir, ec);

        TorrentSessionConfig config;
        StateBridge::instance().applyGlobalLimiters(config);
        config.download_dir = downloadDir;
        config.listen_port = StateBridge::instance().allocatePort();
        config.enable_tracker = parseBoolField(req.form, "enable_tracker", true);
        config.enable_lsd = true;
        config.enable_dht = parseBoolField(req.form, "enable_dht", true);
        const auto [defaultDhtHost, defaultDhtPort] = loadDhtBootstrapConfig();
        std::tie(config.dht_bootstrap_host, config.dht_bootstrap_port) = withTrackerBootstrapFallback(
            req.form.has_field("dht_bootstrap_host") ? req.form.get_field("dht_bootstrap_host") : defaultDhtHost,
            req.form.has_field("dht_bootstrap_port") ? parseUint16Field(req.form, "dht_bootstrap_port", 0) : defaultDhtPort,
            magnetFirstTracker(magnetUri));
        if (config.enable_dht) {
            config.shared_dht = StateBridge::instance().getOrCreateSharedDht(
                6881, config.dht_bootstrap_host, config.dht_bootstrap_port);
        }

        network::MagnetResolveOptions resolveOptions;
        resolveOptions.peer_id = TrackerClient::makePeerId();
        resolveOptions.listen_port = config.listen_port;
        resolveOptions.enable_trackers = config.enable_tracker;
        resolveOptions.enable_dht = config.enable_dht;
        resolveOptions.dht_bootstrap_host = config.dht_bootstrap_host;
        resolveOptions.dht_bootstrap_port = config.dht_bootstrap_port;
        resolveOptions.dht_port = config.listen_port;
        config.client_peer_id = resolveOptions.peer_id;

        TorrentFileMetadata metadata = network::resolve_magnet_uri(magnetUri, resolveOptions);
        const std::uint64_t pieceLength = static_cast<std::uint64_t>(metadata.pieceLength);
        if (metadata.length == 0 || pieceLength == 0 ||
            metadata.pieces.size() != (metadata.length + pieceLength - 1) / pieceLength) {
            res.status = 400;
            res.set_content("Invalid magnet metadata: piece count does not match file length", "text/plain");
            return;
        }

        const std::string infoHashHex = sha1Hex(metadata.infoRaw);
        if (!StateBridge::instance().getSession(infoHashHex)) {
            auto session = std::make_shared<TorrentSession>(metadata, config);
            if (!session->start()) {
                res.status = 503;
                res.set_content("Failed to start torrent session", "text/plain");
                return;
            }
            StateBridge::instance().addSession(session, magnetUri);
        }

        res.status = 200;
        res.set_content("{\"status\":\"ok\",\"info_hash\":\"" + infoHashHex + "\"}",
            "application/json");
    } catch (const std::exception& error) {
        res.status = 400;
        res.set_content(std::string("Failed to resolve magnet: ") + error.what(), "text/plain");
    }
}

void handleRemoveTorrent(const httplib::Request& req, httplib::Response& res) {
    std::string hash = req.has_param("hash") ? req.get_param_value("hash") : "";
    if (hash.empty() && req.has_param("info_hash")) {
        hash = req.get_param_value("info_hash");
    }
    if (hash.empty()) {
        res.status = 400;
        res.set_content("Missing hash parameter", "text/plain");
        return;
    }

    if (StateBridge::instance().removeSession(hash)) {
        res.status = 200;
        res.set_content("{\"status\":\"ok\"}", "application/json");
    } else {
        res.status = 404;
        res.set_content("Torrent not found", "text/plain");
    }
}

// Đổi trạng thái / giới hạn của 1 torrent đang chạy rồi lưu lại để mở app lần sau còn nguyên.
std::shared_ptr<TorrentSession> findSessionOrReply(const httplib::Request& req, httplib::Response& res, std::string& hashOut) {
    hashOut = req.has_param("hash") ? req.get_param_value("hash") : "";
    if (hashOut.empty() && req.has_param("info_hash")) {
        hashOut = req.get_param_value("info_hash");
    }
    if (hashOut.empty()) {
        res.status = 400;
        res.set_content("Missing hash parameter", "text/plain");
        return nullptr;
    }
    auto session = StateBridge::instance().getSession(hashOut);
    if (!session) {
        res.status = 404;
        res.set_content("Torrent not found", "text/plain");
    }
    return session;
}

void handlePauseTorrent(const httplib::Request& req, httplib::Response& res) {
    std::string hash;
    auto session = findSessionOrReply(req, res, hash);
    if (!session) return;
    StateBridge::instance().userPause(hash);
    res.set_content("{\"status\":\"ok\",\"paused\":true}", "application/json");
}

void handleResumeTorrent(const httplib::Request& req, httplib::Response& res) {
    std::string hash;
    auto session = findSessionOrReply(req, res, hash);
    if (!session) return;
    StateBridge::instance().userResume(hash);
    res.set_content("{\"status\":\"ok\",\"paused\":false}", "application/json");
}

// Giới hạn băng thông tổng của cả ứng dụng, KiB/s (0 = không giới hạn). Tham số vắng mặt thì giữ nguyên.
void handleGetBandwidthSettings(const httplib::Request& /*req*/, httplib::Response& res) {
    auto& bridge = StateBridge::instance();
    res.set_content("{\"down\":" + std::to_string(bridge.globalDownloadLimit() / 1024) +
                    ",\"up\":" + std::to_string(bridge.globalUploadLimit() / 1024) + "}",
                    "application/json");
}

void handleSetBandwidthSettings(const httplib::Request& req, httplib::Response& res) {
    auto& bridge = StateBridge::instance();
    constexpr unsigned long long kMaxKiBps = 1024ULL * 1024ULL;  // 1 GiB/s
    unsigned long long values[2] = {bridge.globalDownloadLimit() / 1024, bridge.globalUploadLimit() / 1024};
    const char* keys[2] = {"down", "up"};
    for (int i = 0; i < 2; ++i) {
        if (!req.has_param(keys[i])) continue;
        const std::string text = req.get_param_value(keys[i]);
        if (text.empty() || text.size() > 8 || text.find_first_not_of("0123456789") != std::string::npos ||
            std::stoull(text) > kMaxKiBps) {
            res.status = 400;
            res.set_content(std::string(keys[i]) + " must be a whole number of KiB/s from 0 to 1048576", "text/plain");
            return;
        }
        values[i] = std::stoull(text);
    }
    bridge.setGlobalLimits(values[0] * 1024, values[1] * 1024);
    handleGetBandwidthSettings(req, res);
}

// Hàng đợi tải: tối đa N torrent tải cùng lúc (0 = không giới hạn).
void handleGetQueueSettings(const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_content("{\"max_active\":" + std::to_string(StateBridge::instance().maxActiveDownloads()) + "}",
                    "application/json");
}

void handleSetQueueSettings(const httplib::Request& req, httplib::Response& res) {
    const std::string text = req.has_param("max_active") ? req.get_param_value("max_active") : "";
    if (text.empty() || text.size() > 3 || text.find_first_not_of("0123456789") != std::string::npos ||
        std::stoul(text) > 64) {
        res.status = 400;
        res.set_content("max_active must be an integer from 0 to 64", "text/plain");
        return;
    }
    StateBridge::instance().setMaxActiveDownloads(std::stoul(text));
    handleGetQueueSettings(req, res);
}

// down / up tính bằng KiB/s; 0 = không giới hạn. Tham số vắng mặt thì giữ nguyên giá trị cũ.
void handleLimitTorrent(const httplib::Request& req, httplib::Response& res) {
    std::string hash;
    auto session = findSessionOrReply(req, res, hash);
    if (!session) return;
    constexpr unsigned long long kMaxKiBps = 1024ULL * 1024ULL;  // 1 GiB/s: đủ lớn, chặn tràn số
    for (const char* key : {"down", "up"}) {
        if (!req.has_param(key)) continue;
        unsigned long long kib = 0;
        try {
            const std::string text = req.get_param_value(key);
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
                throw std::invalid_argument("not a number");
            }
            kib = std::stoull(text);
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(std::string("Invalid ") + key + " value", "text/plain");
            return;
        }
        if (kib > kMaxKiBps) {
            res.status = 400;
            res.set_content(std::string("Value of ") + key + " is too large", "text/plain");
            return;
        }
        if (std::string(key) == "down") session->set_download_limit(kib * 1024ULL);
        else session->set_upload_limit(kib * 1024ULL);
    }
    StateBridge::instance().persistState(hash);
    res.set_content("{\"status\":\"ok\"}", "application/json");
}

// GET: danh sách file của torrent kèm trạng thái chọn và tiến độ từng file.
void handleGetTorrentFiles(const httplib::Request& req, httplib::Response& res) {
    std::string hash;
    auto session = findSessionOrReply(req, res, hash);
    if (!session) return;
    const auto files = session->files_status();
    std::string json = "[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i > 0) json += ",";
        json += "{\"index\":" + std::to_string(i) +
                ",\"path\":\"" + jsonEscape(files[i].path) + "\"" +
                ",\"length\":" + std::to_string(files[i].length) +
                ",\"completed_bytes\":" + std::to_string(files[i].completed_bytes) +
                ",\"wanted\":" + (files[i].wanted ? "true" : "false") + "}";
    }
    json += "]";
    res.set_content(json, "application/json");
}

// POST ?hash=...&skip=0,2,5 : các file có chỉ số trong skip bị bỏ chọn, còn lại được tải. skip rỗng = tải hết.
void handleSetTorrentFiles(const httplib::Request& req, httplib::Response& res) {
    std::string hash;
    auto session = findSessionOrReply(req, res, hash);
    if (!session) return;

    std::vector<bool> wanted(session->file_selection().size(), true);
    if (req.has_param("skip")) {
        std::stringstream list(req.get_param_value("skip"));
        std::string item;
        while (std::getline(list, item, ',')) {
            if (item.empty()) continue;
            if (item.size() > 9 || item.find_first_not_of("0123456789") != std::string::npos) {
                res.status = 400;
                res.set_content("Invalid skip list", "text/plain");
                return;
            }
            const std::size_t index = static_cast<std::size_t>(std::stoul(item));
            if (index >= wanted.size()) {
                res.status = 400;
                res.set_content("File index out of range", "text/plain");
                return;
            }
            wanted[index] = false;
        }
    }
    if (!session->set_file_selection(wanted)) {
        res.status = 400;
        res.set_content("Invalid selection: at least one file must stay selected", "text/plain");
        return;
    }
    StateBridge::instance().persistState(hash);
    res.set_content("{\"status\":\"ok\"}", "application/json");
}

// Dựng lại các torrent đã lưu từ lần chạy trước (file đã tải được kiểm tra lại bằng hash khi session khởi tạo).
void restoreSavedTorrents() {
    std::filesystem::path downloadDir = resolveDownloadDir();
    const auto [defaultDhtHost, defaultDhtPort] = loadDhtBootstrapConfig();
    for (auto& saved : StateBridge::instance().loadSavedTorrents()) {
        try {
            const std::string hash = sha1Hex(saved.metadata.infoRaw);
            if (StateBridge::instance().getSession(hash)) continue;

            TorrentSessionConfig config;
            StateBridge::instance().applyGlobalLimiters(config);
            config.download_dir = downloadDir;
            config.listen_port = StateBridge::instance().allocatePort();
            config.enable_tracker = true;
            config.enable_lsd = true;
            config.enable_dht = true;
            config.max_download_rate = saved.download_limit;
            config.max_upload_rate = saved.upload_limit;
            std::tie(config.dht_bootstrap_host, config.dht_bootstrap_port) =
                withTrackerBootstrapFallback(defaultDhtHost, defaultDhtPort, saved.metadata.announce);
            config.shared_dht = StateBridge::instance().getOrCreateSharedDht(
                6881, config.dht_bootstrap_host, config.dht_bootstrap_port);

            auto session = std::make_shared<TorrentSession>(saved.metadata, config);
            if (!session->start()) {
                std::cerr << "[STATE] Could not restart saved torrent " << saved.metadata.name << std::endl;
                continue;
            }
            if (!saved.skipped_files.empty()) {
                std::vector<bool> wanted(session->file_selection().size(), true);
                for (std::size_t index : saved.skipped_files) {
                    if (index < wanted.size()) wanted[index] = false;
                }
                session->set_file_selection(wanted);  // lựa chọn hỏng (bỏ hết file) thì giữ mặc định: tải hết
            }
            if (saved.paused) session->pause();
            StateBridge::instance().addSession(session, saved.magnet_uri);
        } catch (const std::exception& e) {
            std::cerr << "[STATE] Failed to restore torrent: " << e.what() << std::endl;
        }
    }
}

std::string getMimeType(const std::string& extension) {
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".wav") return "audio/wav";
    if (ext == ".ogg") return "audio/ogg";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".doc") return "application/msword";
    if (ext == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == ".xls") return "application/vnd.ms-excel";
    if (ext == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext == ".ppt") return "application/vnd.ms-powerpoint";
    if (ext == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".csv") return "text/csv; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".xml") return "application/xml";
    if (ext == ".zip") return "application/zip";
    if (ext == ".rar") return "application/x-rar-compressed";
    if (ext == ".7z") return "application/x-7z-compressed";
    if (ext == ".tar" || ext == ".gz") return "application/gzip";
    return "application/octet-stream";
}

void handleFileContent(const httplib::Request& req, httplib::Response& res) {
    std::string name;
    if (req.has_param("name")) {
        name = req.get_param_value("name");
    } else if (req.has_param("file")) {
        name = req.get_param_value("file");
    }
    if (name.empty()) {
        res.status = 400;
        res.set_content("Missing file name", "text/plain");
        return;
    }

    const std::filesystem::path safeName = safeFileName(name);
    const std::filesystem::path downloadDir = resolveDownloadDir();
    const std::filesystem::path filePath = downloadDir / safeName;

    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
        res.status = 404;
        res.set_content("File not found", "text/plain");
        return;
    }

    const std::string ext = filePath.extension().string();
    const std::string mime = getMimeType(ext);

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        res.status = 500;
        res.set_content("Failed to read file", "text/plain");
        return;
    }

    const auto fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string buffer;
    buffer.resize(static_cast<std::size_t>(fileSize));
    file.read(&buffer[0], fileSize);

    res.status = 200;
    res.set_header("Content-Disposition", "inline; filename=\"" + asciiFallbackName(safeName.string()) + "\"");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(std::move(buffer), mime);
}

// Mở file/thư mục bằng ứng dụng mặc định của hệ điều hành mà KHÔNG đi qua shell, nên tên file chứa ký tự
// đặc biệt (dấu nháy, $(), `, &...) không thể chèn lệnh.
bool openWithDefaultApp(const std::filesystem::path& target) {
#if defined(_WIN32)
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    const std::string path = target.string();  // đường dẫn tuyệt đối bắt đầu bằng '/', không bị hiểu là tùy chọn
    char* argv[] = {const_cast<char*>(opener), const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0) {
        return false;
    }
    std::thread([pid]() {
        int status = 0;
        waitpid(pid, &status, 0);  // thu dọn tiến trình con, tránh zombie
    }).detach();
    return true;
#endif
}

void handleFileOpen(const httplib::Request& req, httplib::Response& res) {
    const std::filesystem::path downloadDir = resolveDownloadDir();
    std::filesystem::path targetPath;

    if (req.has_param("folder") && req.get_param_value("folder") == "1") {
        targetPath = downloadDir;
    } else {
        std::string name;
        if (req.has_param("name")) {
            name = req.get_param_value("name");
        } else if (!req.body.empty()) {
            if (req.is_multipart_form_data() && req.form.has_field("name")) {
                name = req.form.get_field("name");
            } else {
                auto pos = req.body.find("\"name\"");
                if (pos != std::string::npos) {
                    auto start = req.body.find('"', pos + 6);
                    if (start != std::string::npos) {
                        auto end = req.body.find('"', start + 1);
                        if (end != std::string::npos) {
                            name = req.body.substr(start + 1, end - start - 1);
                        }
                    }
                }
            }
        }

        if (name.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Missing file name\"}", "application/json");
            return;
        }

        const std::filesystem::path safeName = safeFileName(name);
        targetPath = downloadDir / safeName;
        std::error_code statusEc;
        const auto symlinkStatus = std::filesystem::symlink_status(targetPath, statusEc);
        if (statusEc || !std::filesystem::exists(symlinkStatus)) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"message\":\"File does not exist on disk\"}", "application/json");
            return;
        }
        // Chỉ mở đúng mục nằm trực tiếp trong thư mục tải về: không theo symlink ra ngoài.
        if (std::filesystem::is_symlink(symlinkStatus)) {
            res.status = 403;
            res.set_content("{\"status\":\"error\",\"message\":\"Refusing to open a symbolic link\"}", "application/json");
            return;
        }
        // Torrent là dữ liệu của người lạ: tên file do họ đặt và nội dung có thể là mã độc. Chỉ mở loại file
        // không thực thi được; thư mục (gốc torrent nhiều file) mở bằng trình quản lý file.
        const bool isDirectory = std::filesystem::is_directory(symlinkStatus);
        const std::string fileName = safeName.string();
        const bool allowed = isDirectory ? webui::isOpenableDirectoryName(fileName)
                                         : webui::isOpenableFileName(fileName);
        if (!allowed) {
            res.status = 403;
            res.set_content("{\"status\":\"error\",\"message\":\"This file type cannot be opened from the web UI. "
                            "Open the download folder instead.\"}", "application/json");
            return;
        }
    }

    if (!openWithDefaultApp(targetPath)) {
        res.status = 500;
        res.set_content("{\"status\":\"error\",\"message\":\"Could not open the file\"}", "application/json");
        return;
    }

    res.status = 200;
    res.set_content("{\"status\":\"ok\",\"message\":\"Opened " + jsonEscape(targetPath.filename().string()) + "\"}", "application/json");
}

}  // namespace

void ApiRoutes::registerRoutes(httplib::Server& server, const std::filesystem::path& /*tempDir*/) {
    // Giới hạn body trước khi httplib đọc vào RAM (file + vài field form).
    server.set_payload_max_length(kMaxUploadBytes + 1024 * 1024);

    // Tự động khởi động node DHT dùng chung (UDP 6881) ngay khi server chạy
    const auto [bootHost, bootPort] = loadDhtBootstrapConfig();
    StateBridge::instance().getOrCreateSharedDht(6881, bootHost, bootPort);

    // Khôi phục danh sách torrent của lần chạy trước. Chạy nền vì mỗi session phải kiểm tra lại hash file đã tải.
    StateBridge::instance().setStateDir(resolveDownloadDir() / ".p2p_state");
    std::thread([]() { restoreSavedTorrents(); }).detach();

    server.Post("/api/torrents/create", [](const httplib::Request& req, httplib::Response& res) {
        handleCreateTorrent(req, res);
    });

    server.Post("/api/torrents/add", [](const httplib::Request& req, httplib::Response& res) {
        handleAddTorrent(req, res);
    });

    server.Post("/api/torrents/add-magnet", [](const httplib::Request& req, httplib::Response& res) {
        handleAddMagnet(req, res);
    });

    server.Post("/api/torrents/remove", [](const httplib::Request& req, httplib::Response& res) {
        handleRemoveTorrent(req, res);
    });

    server.Post("/api/torrents/pause", [](const httplib::Request& req, httplib::Response& res) {
        handlePauseTorrent(req, res);
    });

    server.Post("/api/torrents/resume", [](const httplib::Request& req, httplib::Response& res) {
        handleResumeTorrent(req, res);
    });

    server.Get("/api/settings/bandwidth", [](const httplib::Request& req, httplib::Response& res) {
        handleGetBandwidthSettings(req, res);
    });

    server.Post("/api/settings/bandwidth", [](const httplib::Request& req, httplib::Response& res) {
        handleSetBandwidthSettings(req, res);
    });

    server.Get("/api/settings/queue", [](const httplib::Request& req, httplib::Response& res) {
        handleGetQueueSettings(req, res);
    });

    server.Post("/api/settings/queue", [](const httplib::Request& req, httplib::Response& res) {
        handleSetQueueSettings(req, res);
    });

    server.Get("/api/torrents/files", [](const httplib::Request& req, httplib::Response& res) {
        handleGetTorrentFiles(req, res);
    });

    server.Post("/api/torrents/files", [](const httplib::Request& req, httplib::Response& res) {
        handleSetTorrentFiles(req, res);
    });

    server.Post("/api/torrents/limit", [](const httplib::Request& req, httplib::Response& res) {
        handleLimitTorrent(req, res);
    });

    server.Get("/api/torrents", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(StateBridge::instance().toJsonString(), "application/json");
    });

    server.Get("/api/files/content", [](const httplib::Request& req, httplib::Response& res) {
        handleFileContent(req, res);
    });

    server.Post("/api/files/open", [](const httplib::Request& req, httplib::Response& res) {
        handleFileOpen(req, res);
    });

    server.Get("/api/status", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(StateBridge::instance().toJsonString(), "application/json");
    });

    server.Get("/api/system/info", [](const httplib::Request& /*req*/, httplib::Response& res) {
        const std::string ip = getLocalLanIp();
        const std::string announce = "http://" + ip + ":6969/announce";
        std::string json = "{\"ip\":\"" + ip + "\",\"default_announce\":\"" + announce + "\"}";
        res.set_content(json, "application/json");
    });

    server.Get("/api/dht/bootstrap", [](const httplib::Request& /*req*/, httplib::Response& res) {
        const auto [host, port] = loadDhtBootstrapConfig();
        const std::string configPath = resolveDhtConfigPath().string();
        std::string json = "{\"host\":\"" + jsonEscape(host) + "\",\"port\":" + std::to_string(port) +
                           ",\"source\":\"" + getDhtBootstrapSource() + "\",\"config_path\":\"" + jsonEscape(configPath) + "\"}";
        res.set_content(json, "application/json");
    });

    server.Post("/api/dht/bootstrap", [](const httplib::Request& req, httplib::Response& res) {
        const auto [host, port] = extractDhtBootstrapFromRequest(req);
        if (host.empty() || port == 0) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Requires host and port\"}", "application/json");
            return;
        }

        saveDhtBootstrapConfig(host, port);
        auto sharedDht = StateBridge::instance().getOrCreateSharedDht(6881, host, port);
        if (sharedDht) {
            std::thread([sharedDht, h = host, p = port]() {
                sharedDht->bootstrap(h, p);
            }).detach();
        }

        std::string json = "{\"status\":\"ok\",\"host\":\"" + jsonEscape(host) + "\",\"port\":" + std::to_string(port) +
                           ",\"config_path\":\"" + jsonEscape(resolveDhtConfigPath().string()) + "\"}";
        res.set_content(json, "application/json");
    });

    server.Get("/api/dht/status", [](const httplib::Request& /*req*/, httplib::Response& res) {
        const auto [host, port] = loadDhtBootstrapConfig();
        const auto sharedDht = StateBridge::instance().sharedDht();
        const bool running = sharedDht != nullptr;
        const std::size_t nodesCount = sharedDht ? sharedDht->routingTable().size() : 0;
        const std::uint16_t dhtPort = sharedDht ? sharedDht->port() : 0;
        std::string json = "{\"enabled\":true"
                           ",\"running\":" + std::string(running ? "true" : "false") +
                           ",\"port\":" + std::to_string(dhtPort) +
                           ",\"nodes_count\":" + std::to_string(nodesCount) +
                           ",\"bootstrap_host\":\"" + jsonEscape(host) + "\",\"bootstrap_port\":" + std::to_string(port) +
                           ",\"source\":\"" + getDhtBootstrapSource() + "\",\"config_path\":\"" + jsonEscape(resolveDhtConfigPath().string()) + "\"}";
        res.set_content(json, "application/json");
    });
}

}  // namespace p2p
