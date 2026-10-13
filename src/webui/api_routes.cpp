#include "webui/api_routes.h"

#include "httplib.h"

#include "core/hash_utils.h"
#include "core/torrent_builder.h"
#include "core/torrent_file.h"
#include "webui/state_bridge.h"

#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>

namespace p2p {
namespace {

constexpr const char* kDefaultAnnounce = "http://127.0.0.1:6969/announce";
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

    std::filesystem::path downloadDir = std::filesystem::current_path() / "data" / "downloads";
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
        : kDefaultAnnounce;
    const std::size_t pieceLength = parsePieceLength(req.form);
    const std::string nameOverride = req.form.has_field("name") ? req.form.get_field("name") : "";

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
        config.download_dir = downloadDir;
        config.listen_port = StateBridge::instance().allocatePort();
        config.enable_lsd = true;

        auto session = std::make_shared<TorrentSession>(meta, config);
        session->start();
        StateBridge::instance().addSession(session, magnetUri);

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
        std::string infoHashHex = sha1Hex(meta.infoRaw);
        std::string magnetUri = "magnet:?xt=urn:btih:" + infoHashHex +
                                "&dn=" + rfc5987Encode(meta.name) +
                                "&tr=" + rfc5987Encode(meta.announce);

        std::filesystem::path downloadDir = std::filesystem::current_path() / "data" / "downloads";
        std::error_code ec;
        std::filesystem::create_directories(downloadDir, ec);

        TorrentSessionConfig config;
        config.download_dir = downloadDir;
        config.listen_port = StateBridge::instance().allocatePort();
        config.enable_lsd = true;

        auto session = std::make_shared<TorrentSession>(meta, config);
        session->start();
        StateBridge::instance().addSession(session, magnetUri);

        res.status = 200;
        res.set_content("{\"status\":\"ok\",\"info_hash\":\"" + infoHashHex + "\"}", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("Failed to parse torrent: ") + e.what(), "text/plain");
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

}  // namespace

void ApiRoutes::registerRoutes(httplib::Server& server, const std::filesystem::path& /*tempDir*/) {
    server.Post("/api/torrents/create", [](const httplib::Request& req, httplib::Response& res) {
        handleCreateTorrent(req, res);
    });

    server.Post("/api/torrents/add", [](const httplib::Request& req, httplib::Response& res) {
        handleAddTorrent(req, res);
    });

    server.Post("/api/torrents/remove", [](const httplib::Request& req, httplib::Response& res) {
        handleRemoveTorrent(req, res);
    });

    server.Get("/api/torrents", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(StateBridge::instance().toJsonString(), "application/json");
    });

    server.Get("/api/status", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(StateBridge::instance().toJsonString(), "application/json");
    });
}

}  // namespace p2p
