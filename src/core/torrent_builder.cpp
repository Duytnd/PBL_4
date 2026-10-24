#include "core/torrent_builder.h"

#include "core/hash_utils.h"
#include "core/path_sanitizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace p2p {
namespace {

// Lấy tên torrent: ưu tiên override nếu có, nếu không dùng tên file/thu mục nguồn.
std::string makeName(const std::filesystem::path& sourcePath, const std::string& nameOverride) {
    if (!nameOverride.empty()) {
        return nameOverride;
    }
    return sourcePath.filename().string();
}

bool isRegularFileSafe(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        return false;
    }
    return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
}

struct FileEntry {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
    std::uintmax_t size = 0;
};

std::vector<FileEntry> collectFiles(const std::filesystem::path& sourcePath) {
    std::vector<FileEntry> files;
    std::error_code ec;

    if (!std::filesystem::exists(sourcePath, ec) || !std::filesystem::is_directory(sourcePath, ec)) {
        if (std::filesystem::exists(sourcePath, ec) && isRegularFileSafe(sourcePath)) {
            files.push_back({sourcePath.filename(), sourcePath, std::filesystem::file_size(sourcePath, ec)});
            return files;
        }
        throw std::invalid_argument("TorrentBuilder: source path must be a readable file or directory");
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(sourcePath, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            continue;
        }
        const auto status = entry.symlink_status(ec);
        if (ec) {
            continue;
        }
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
            continue;
        }

        const auto relative = std::filesystem::relative(entry.path(), sourcePath, ec);
        if (ec) {
            continue;
        }
        if (!relative.empty()) {
            const std::string relativeString = relative.generic_string();
            if (!PathSanitizer::isSafeRelativePath(relativeString)) {
                continue;
            }
            files.push_back({relative, entry.path(), std::filesystem::file_size(entry.path(), ec)});
        }
    }

    std::sort(files.begin(), files.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
        return lhs.relativePath.string() < rhs.relativePath.string();
    });
    return files;
}

// Đọc file streaming theo chunk, hash từng piece mà không load toàn bộ file vào RAM.
// pieceLength thường là 256KB, đọc từng 1MB buffer để tránh tốn RAM với file lớn.
std::string computePieceHashesStreaming(
    const std::vector<FileEntry>& files,
    std::size_t pieceLength)
{
    static constexpr std::size_t kReadBuf = 1024 * 1024; // 1MB read buffer

    std::string pieces;
    Sha1Context pieceCtx;        // SHA-1 context cho piece hiện tại
    std::size_t byteInPiece = 0; // Số byte đã nạp vào piece hiện tại

    std::vector<char> buf(kReadBuf);

    for (const auto& file : files) {
        if (file.size == 0) continue;

        std::ifstream in(file.absolutePath, std::ios::binary);
        if (!in) continue;

        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(kReadBuf));
            std::size_t bytesRead = static_cast<std::size_t>(in.gcount());
            if (bytesRead == 0) break;

            std::size_t pos = 0;
            while (pos < bytesRead) {
                // Số byte còn thiếu để lấp đầy piece hiện tại
                const std::size_t spaceLeft = pieceLength - byteInPiece;
                const std::size_t chunk = std::min(spaceLeft, bytesRead - pos);

                pieceCtx.update(buf.data() + pos, chunk);
                byteInPiece += chunk;
                pos += chunk;

                if (byteInPiece == pieceLength) {
                    // Piece đầy → finalize hash và reset
                    pieces.append(pieceCtx.finalize());
                    pieceCtx = Sha1Context();
                    byteInPiece = 0;
                }
            }
        }
    }

    // Piece cuối (có thể chưa đầy)
    if (byteInPiece > 0) {
        pieces.append(pieceCtx.finalize());
    }

    return pieces;
}

}  // namespace

BencodeDict TorrentBuilder::buildInfoDict(
    const std::filesystem::path& sourcePath,
    std::size_t pieceLength,
    const std::string& nameOverride) {

    if (pieceLength == 0 || (pieceLength & (pieceLength - 1)) != 0) {
        pieceLength = 262144;
    }

    const std::string name = makeName(sourcePath, nameOverride);
    const auto files = collectFiles(sourcePath);

    BencodeDict info;
    info["name"] = BencodeValue(name);
    info["piece length"] = BencodeValue(static_cast<int64_t>(pieceLength));

    // Tính tổng dung lượng và tạo danh sách file (multi-file mode)
    std::uintmax_t totalLength = 0;
    bool isSingleFile = (files.size() == 1 && files[0].relativePath == sourcePath.filename());

    if (!isSingleFile) {
        BencodeList fileList;
        for (const auto& file : files) {
            totalLength += file.size;
            BencodeDict entry;
            entry["length"] = BencodeValue(static_cast<int64_t>(file.size));
            BencodeList pathList;
            const auto relPath = file.relativePath.lexically_normal();
            for (const auto& part : relPath) {
                pathList.emplace_back(BencodeValue(part.string()));
            }
            entry["path"] = BencodeValue(pathList);
            fileList.push_back(BencodeValue(entry));
        }
        info["files"] = BencodeValue(fileList);
    } else {
        totalLength = files[0].size;
    }

    // Hash piece streaming — không load toàn bộ dữ liệu vào RAM
    const std::string piecesBinary = computePieceHashesStreaming(files, pieceLength);

    info["length"] = BencodeValue(static_cast<int64_t>(totalLength));
    info["pieces"] = BencodeValue(piecesBinary);
    return info;
}

std::string TorrentBuilder::buildTorrent(
    const std::filesystem::path& sourcePath,
    const std::string& announce,
    std::size_t pieceLength,
    const std::string& nameOverride) {

    BencodeDict root;
    root["announce"] = BencodeValue(announce);
    root["info"] = BencodeValue(buildInfoDict(sourcePath, pieceLength, nameOverride));
    return encodeBencode(root);
}

std::string TorrentBuilder::computeInfoHash(
    const std::filesystem::path& sourcePath,
    std::size_t pieceLength,
    const std::string& nameOverride) {

    const auto infoDict = buildInfoDict(sourcePath, pieceLength, nameOverride);
    return p2p::computeInfoHash(infoDict);
}

}  // namespace p2p
