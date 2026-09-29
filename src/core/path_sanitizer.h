#pragma once

#include <string>
#include <vector>

namespace p2p {

class PathSanitizer {
public:
    static bool isSafeRelativePath(const std::string& path);
    static std::string normalize(const std::string& path);

private:
    static bool isReservedWindowsName(const std::string& segment);
};

}  // namespace p2p
