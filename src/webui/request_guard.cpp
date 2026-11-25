#include "webui/request_guard.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace p2p::webui {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Tách hostname khỏi "host[:port]", "scheme://host[:port][/path]" hoặc "[ipv6]:port".
std::string extractHost(std::string value) {
    const std::size_t scheme = value.find("://");
    if (scheme != std::string::npos) {
        value.erase(0, scheme + 3);
    }
    const std::size_t slash = value.find_first_of("/?#");
    if (slash != std::string::npos) {
        value.erase(slash);
    }
    const std::size_t at = value.rfind('@');  // bỏ phần userinfo (user:pass@host)
    if (at != std::string::npos) {
        value.erase(0, at + 1);
    }
    if (!value.empty() && value.front() == '[') {
        const std::size_t close = value.find(']');
        return close == std::string::npos ? std::string() : value.substr(1, close - 1);
    }
    const std::size_t colon = value.find(':');
    if (colon != std::string::npos) {
        value.erase(colon);
    }
    return value;
}

// IPv4 literal đúng 4 nhóm số thập phân 0-255, không số 0 đứng đầu mơ hồ ("010.0.0.1" bị từ chối).
bool parseIpv4(const std::string& text, std::array<int, 4>& octets) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t end = i == 3 ? text.size() : text.find('.', pos);
        if (end == std::string::npos || end == pos || end - pos > 3) {
            return false;
        }
        const std::string part = text.substr(pos, end - pos);
        if (!std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return false;
        }
        if (part.size() > 1 && part.front() == '0') {
            return false;
        }
        const int value = std::stoi(part);
        if (value > 255) {
            return false;
        }
        octets[i] = value;
        pos = end + 1;
    }
    return pos == text.size() + 1;
}

}  // namespace

bool isAllowedHost(const std::string& value) {
    const std::string host = toLower(extractHost(value));
    if (host.empty()) {
        return false;
    }
    if (host == "localhost" || host == "::1") {
        return true;
    }
    std::array<int, 4> o{};
    if (!parseIpv4(host, o)) {
        return false;  // tên miền hoặc chuỗi lạ: không bao giờ được tin
    }
    return o[0] == 127 ||
           o[0] == 10 ||
           (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
           (o[0] == 192 && o[1] == 168) ||
           (o[0] == 100 && o[1] >= 64 && o[1] <= 127);
}

bool isLoopbackAddress(const std::string& address) {
    std::string a = toLower(address);
    if (a.size() >= 2 && a.front() == '[' && a.back() == ']') {
        a = a.substr(1, a.size() - 2);
    }
    if (a == "::1") {
        return true;
    }
    if (a.rfind("::ffff:", 0) == 0) {
        a.erase(0, 7);  // IPv4-mapped IPv6
    }
    std::array<int, 4> o{};
    return parseIpv4(a, o) && o[0] == 127;
}

bool isSameOrigin(const std::string& origin, const std::string& hostHeader) {
    if (origin.empty()) {
        return true;
    }
    if (hostHeader.empty()) {
        return false;
    }
    return toLower(origin) == "http://" + toLower(hostHeader);
}

namespace {

bool hasOnlySafeNameChars(const std::string& name) {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        switch (c) {
            case ':': case '<': case '>': case '"': case '|': case '?': case '*':
            case '`': case '$': case ';': case '&': case '%': case '\\': case '/':
                return false;
            default:
                break;
        }
    }
    return true;
}

}  // namespace

bool isOpenableDirectoryName(const std::string& dirName) {
    if (!hasOnlySafeNameChars(dirName) || dirName == "." || dirName == "..") {
        return false;
    }
    return dirName.back() != '.' && dirName.back() != ' ';
}

bool isOpenableFileName(const std::string& fileName) {
    if (!hasOnlySafeNameChars(fileName)) {
        return false;
    }

    // Windows bỏ dấu chấm/khoảng trắng cuối: "evil.exe." hay "evil.exe " vẫn chạy như evil.exe.
    std::string trimmed = fileName;
    while (!trimmed.empty() && (trimmed.back() == '.' || trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed != fileName) {
        return false;
    }
    const std::size_t dot = trimmed.rfind('.');
    if (dot == std::string::npos || dot + 1 >= trimmed.size()) {
        return false;  // không có đuôi: không biết sẽ mở bằng gì
    }
    static constexpr std::string_view kAllowed[] = {
        // ảnh
        "png", "jpg", "jpeg", "gif", "webp", "bmp", "ico",  // không có svg: mở bằng trình duyệt thì script chạy được
        // video
        "mp4", "mov", "webm", "mkv", "avi", "m4v",
        // âm thanh
        "mp3", "wav", "ogg", "flac", "m4a", "aac",
        // tài liệu
        // không có doc/xls/ppt/rtf (định dạng cũ cho phép macro/đối tượng nhúng): file ghi ra không có Mark-of-the-Web
        // nên Office không bật Protected View
        "pdf", "docx", "xlsx", "pptx", "odt", "ods", "odp",
        // văn bản
        "txt", "md", "csv", "json", "log", "xml", "yaml", "yml", "ini",
        // nén (chỉ mở trình xem, không tự giải nén/chạy)
        "zip", "7z", "rar", "gz", "tar", "bz2", "xz"};
    const std::string ext = toLower(trimmed.substr(dot + 1));
    return std::find(std::begin(kAllowed), std::end(kAllowed), ext) != std::end(kAllowed);
}

}  // namespace p2p::webui
