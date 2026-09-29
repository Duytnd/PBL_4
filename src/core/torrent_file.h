#pragma once

#include "core/bencode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct TorrentFileMetadata {
    std::string announce;
    std::string name;
    std::int64_t pieceLength = 0;
    std::vector<std::string> pieces;
    std::uint64_t length = 0;
    std::vector<std::string> filePaths;
    std::string infoRaw;

    [[nodiscard]] bool hasInfo() const {
        return !infoRaw.empty();
    }
};

class TorrentFile {
public:
    static TorrentFileMetadata parse(const std::string& encodedData);
    static TorrentFileMetadata parseFile(const std::string& filePath);
};

}  // namespace p2p
