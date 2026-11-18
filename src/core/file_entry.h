#pragma once

#include <cstdint>
#include <string>

namespace p2p {

// Một file trong torrent multi-file (BEP 3): đường dẫn tương đối (dùng '/') và độ dài.
// Các file được nối liên tiếp theo thứ tự trong danh sách để tạo luồng byte chia piece.
struct FileEntry {
    std::string path;
    std::uint64_t length = 0;
};

}  // namespace p2p
