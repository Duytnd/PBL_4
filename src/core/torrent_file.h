#pragma once

#include "core/bencode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

// Lưu trữ thông tin đã parse từ file .torrent.
// Quy ước: announce là tracker, name là tên file, pieces là danh sách hash từng piece.
struct TorrentFileMetadata {
    std::string announce;              // Tracker URL, ví dụ: http://tracker.example:6969/announce
    std::string name;                  // Tên file hoặc tên thư mục trong torrent
    std::int64_t pieceLength = 0;      // Kích thước mỗi piece, thường 256 KiB
    std::vector<std::string> pieces;   // SHA-1 hash của từng piece, mỗi hash dài 20 byte
    std::uint64_t length = 0;          // Tổng kích thước file nếu single-file
    std::vector<std::string> filePaths; // Danh sách đường dẫn file nếu multi-file
    std::string infoRaw;               // Raw Bencode của phần "info" để tính info hash

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
