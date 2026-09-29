#include "core/path_sanitizer.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace p2p {
namespace {

// Kiểm tra xem path có bắt đầu bằng ổ đĩa Windows (ví dụ: C:/...) hay không.
// Điều này bị chặn vì trong torrent/file system không nên chấp nhận đường dẫn tuyệt đối.
bool isWindowsDrivePrefix(const std::string& path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return true;
    }
    return false;
}

bool containsNul(const std::string& path) {
    return path.find('\0') != std::string::npos;
}

std::string trim(std::string value) {
    const auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> splitSegments(const std::string& path) {
    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            segments.push_back(item);
        }
    }
    return segments;
}

}  // namespace

bool PathSanitizer::isReservedWindowsName(const std::string& segment) {
    static const std::set<std::string> reserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    std::string upper = segment;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return reserved.count(upper) > 0;
}

bool PathSanitizer::isSafeRelativePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (containsNul(path)) {
        return false;
    }
    if (path == "." || path == "..") {
        return false;
    }
    if (path.front() == '/' || path.back() == '/') {
        return false;
    }
    if (path.find('\\') != std::string::npos) {
        return false;
    }
    if (isWindowsDrivePrefix(path)) {
        return false;
    }

    std::vector<std::string> segments = splitSegments(path);
    if (segments.empty()) {
        return false;
    }

    for (const auto& segment : segments) {
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (segment == "" || segment.find('\0') != std::string::npos) {
            return false;
        }
        if (segment.size() > 255) {
            return false;
        }
        if (isReservedWindowsName(segment)) {
            return false;
        }
    }

    return true;
}

std::string PathSanitizer::normalize(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    if (containsNul(path)) {
        return "";
    }
    if (path == "." || path == "..") {
        return "";
    }
    if (path.front() == '/' || path.back() == '/') {
        return "";
    }
    if (path.find('\\') != std::string::npos) {
        return "";
    }
    if (isWindowsDrivePrefix(path)) {
        return "";
    }

    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (item.empty() || item == ".") {
            continue;
        }
        if (item == "..") {
            return "";
        }
        const std::string trimmed = trim(item);
        if (trimmed.empty() || trimmed == "." || trimmed == "..") {
            return "";
        }
        if (containsNul(trimmed)) {
            return "";
        }
        if (isReservedWindowsName(trimmed)) {
            return "";
        }
        parts.push_back(trimmed);
    }

    if (parts.empty()) {
        return "";
    }

    std::string normalized;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized += '/';
        }
        normalized += parts[i];
    }
    return normalized;
}

}  // namespace p2p
