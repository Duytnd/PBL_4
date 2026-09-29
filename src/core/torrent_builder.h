#pragma once

#include "core/bencode.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace p2p {

// Tạo metadata torrent từ file hoặc thư mục nguồn.
// Chuyển dữ liệu đầu vào thành cấu trúc Bencode của "info" và "root".
class TorrentBuilder {
public:
    static std::string buildTorrent(
        const std::filesystem::path& sourcePath,
        const std::string& announce,
        std::size_t pieceLength = 262144,
        const std::string& nameOverride = "");

    static BencodeDict buildInfoDict(
        const std::filesystem::path& sourcePath,
        std::size_t pieceLength = 262144,
        const std::string& nameOverride = "");

    static std::string computeInfoHash(
        const std::filesystem::path& sourcePath,
        std::size_t pieceLength = 262144,
        const std::string& nameOverride = "");
};

}  // namespace p2p
