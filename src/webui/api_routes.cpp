#include "webui/api_routes.h"

#include "httplib.h"

#include "core/torrent_builder.h"

#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>

namespace p2p {
namespace {

constexpr const char* kDefaultAnnounce = "http://127.0.0.1:6969/announce";
constexpr std::size_t kDefaultPieceLength = 262144;
// Giới hạn upload cho /api/torrents/create: tránh một request khổng lồ làm
// nghẽn thread HTTP (mục 5.7 "giới hạn phòng thủ").
constexpr std::size_t kMaxUploadBytes = 256ull * 1024 * 1024;

// Phần trăm-encode theo RFC 5987 (dùng cho filename*=UTF-8''...), vì header
// HTTP không phải UTF-8: ký tự ngoài ASCII (vd tiếng Việt có dấu) phải được
// encode, nếu không trình duyệt hiển thị sai (lỗi font/mojibake).
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

// Tên file an toàn cho phần filename="..." (ASCII) khi trình duyệt cũ không
// hiểu filename*=UTF-8''...: thay mọi byte ngoài ASCII in được bằng '_'.
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

// Chỉ giữ lại tên file (bỏ mọi phần đường dẫn người dùng gửi lên) để tránh
// path traversal khi ghi file tạm ra đĩa.
std::filesystem::path safeFileName(const std::string& rawName) {
    std::filesystem::path name = std::filesystem::path(rawName).filename();
    if (name.empty() || name == "." || name == "..") {
        name = "upload.bin";
    }
    return name;
}

std::filesystem::path makeUniqueUploadDir(const std::filesystem::path& tempDir) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<unsigned long long> dist;

    const std::string folder = "upload_" + std::to_string(now) + "_" + std::to_string(dist(rng));
    return tempDir / folder;
}

// Xoá thư mục tạm khi ra khỏi scope, dù thành công hay lỗi.
struct ScopedTempDir {
    std::filesystem::path path;
    ~ScopedTempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

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

void handleCreateTorrent(const httplib::Request& req, httplib::Response& res,
                          const std::filesystem::path& tempDir) {
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

    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    ScopedTempDir scopedDir{makeUniqueUploadDir(tempDir)};
    std::filesystem::create_directories(scopedDir.path, ec);
    if (ec) {
        res.status = 500;
        res.set_content("Failed to prepare upload directory", "text/plain");
        return;
    }

    const std::filesystem::path sourceName = safeFileName(upload.filename);
    const std::filesystem::path sourcePath = scopedDir.path / sourceName;

    {
        std::ofstream output(sourcePath, std::ios::binary);
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
        // Chỗ nối thật: dùng đúng TorrentBuilder của tầng core (5.1), không
        // phải bản bencode/SHA-1 viết lại bằng JavaScript phía client nữa.
        const std::string torrentBytes = TorrentBuilder::buildTorrent(
            sourcePath, announce, pieceLength, nameOverride);

        const std::string downloadName =
            (nameOverride.empty() ? sourceName.string() : nameOverride) + ".torrent";

        res.set_header("Content-Disposition", buildContentDisposition(downloadName));
        res.set_content(torrentBytes, "application/x-bittorrent");
    } catch (const std::exception& error) {
        res.status = 500;
        res.set_content(std::string("Failed to build torrent: ") + error.what(), "text/plain");
    }
}

}  // namespace

void ApiRoutes::registerRoutes(httplib::Server& server, const std::filesystem::path& tempDir) {
    server.Post("/api/torrents/create", [tempDir](const httplib::Request& req, httplib::Response& res) {
        handleCreateTorrent(req, res, tempDir);
    });
}

}  // namespace p2p
