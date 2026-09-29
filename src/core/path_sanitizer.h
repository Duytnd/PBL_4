#pragma once

#include <string>
#include <vector>

namespace p2p {

// Chặn path độc hại để tránh traversal, ký tự null, tên file Windows hệ thống,
// hoặc các đường dẫn tương đối không an toàn khi tạo torrent hoặc lưu file.
class PathSanitizer {
public:
    static bool isSafeRelativePath(const std::string& path);
    static std::string normalize(const std::string& path);

private:
    static bool isReservedWindowsName(const std::string& segment);
};

}  // namespace p2p
