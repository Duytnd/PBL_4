#pragma once

#include "core/bencode.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace p2p {

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
