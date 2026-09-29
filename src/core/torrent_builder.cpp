#include "core/torrent_builder.h"

#include "core/hash_utils.h"
#include "core/path_sanitizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::string encodePieceHashes(const std::string& payload, std::size_t pieceLength) {
    std::string pieces;
    for (std::size_t offset = 0; offset < payload.size(); offset += pieceLength) {
        const std::size_t chunkSize = std::min<std::size_t>(pieceLength, payload.size() - offset);
        const std::string piece = payload.substr(offset, chunkSize);
        const std::string digest = sha1Digest(piece);
        pieces.append(digest);
    }
    return pieces;
}

std::string buildSingleFilePayload(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("TorrentBuilder: cannot read source file");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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

    std::string piecesBinary;
    std::uintmax_t totalLength = 0;
    std::string payload;

    if (files.size() == 1 && files[0].relativePath == sourcePath.filename()) {
        payload = buildSingleFilePayload(files[0].absolutePath);
        totalLength = payload.size();
    } else {
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

            if (file.size > 0) {
                std::ifstream input(file.absolutePath, std::ios::binary);
                if (!input) {
                    continue;
                }
                payload.append(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
            }
        }
        info["files"] = BencodeValue(fileList);
    }

    if (files.size() == 1 && files[0].relativePath == sourcePath.filename()) {
        piecesBinary = encodePieceHashes(payload, pieceLength);
    } else {
        piecesBinary = encodePieceHashes(payload, pieceLength);
    }

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
