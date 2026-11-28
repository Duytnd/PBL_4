#include "webui/folder_upload.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

#include "core/path_sanitizer.h"

namespace p2p::webui {
namespace fs = std::filesystem;

namespace {

struct Entry {
    std::vector<std::string> parts;  // đã bỏ thành phần gốc
    std::string_view content;
};

// Tách theo '/' và từ chối thành phần rỗng ("a//b"): PathSanitizer bỏ qua thành phần rỗng
// nên "a//b" và "a/b" sẽ trùng nhau mà không bị phát hiện.
bool splitStrict(const std::string& path, std::vector<std::string>& out) {
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty()) {
            return false;
        }
        out.push_back(part);
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string joinParts(const std::vector<std::string>& parts, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out += '/';
        }
        out += parts[i];
    }
    return out;
}

// Đường dẫn cuối cùng phải nằm dưới root (so sánh theo thành phần sau lexically_normal).
bool isUnderRoot(const fs::path& root, const fs::path& target) {
    std::vector<fs::path> rootParts;
    for (const auto& part : root.lexically_normal()) {
        if (!part.empty()) {
            rootParts.push_back(part);
        }
    }
    std::vector<fs::path> targetParts;
    for (const auto& part : target.lexically_normal()) {
        if (!part.empty()) {
            targetParts.push_back(part);
        }
    }
    if (targetParts.size() <= rootParts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rootParts.size(); ++i) {
        if (rootParts[i] != targetParts[i]) {
            return false;
        }
    }
    for (std::size_t i = rootParts.size(); i < targetParts.size(); ++i) {
        if (targetParts[i] == "..") {
            return false;
        }
    }
    return true;
}

void removeQuietly(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

StagedFolder stageFolderUpload(const fs::path& downloadDir,
                               const std::vector<UploadedFile>& files,
                               const std::string& nameOverride,
                               const FolderUploadOptions& options) {
    // Luật 1: giới hạn số file và tổng byte, kiểm tra trước khi ghi bất cứ thứ gì.
    if (files.empty()) {
        throw FolderUploadError(400, "No files in folder upload");
    }
    if (files.size() > options.maxFiles) {
        throw FolderUploadError(413, "Too many files in folder");
    }
    std::uint64_t receivedBytes = 0;
    for (const auto& file : files) {
        receivedBytes += file.content.size();
        if (receivedBytes > options.maxBytes) {
            throw FolderUploadError(413, "Folder too large");
        }
    }

    // Luật 3 (phần tên override): đúng một thành phần, an toàn.
    const bool hasOverride = !nameOverride.empty();
    if (hasOverride &&
        (nameOverride.find('/') != std::string::npos || !PathSanitizer::isSafeRelativePath(nameOverride))) {
        throw FolderUploadError(400, "Invalid torrent name");
    }

    // Luật 2 + 3 + 4: kiểm tra từng đường dẫn, xác định thư mục gốc chung, bỏ mục ẩn.
    std::string rootName;
    std::vector<Entry> kept;
    std::size_t skippedHidden = 0;
    bool firstPath = true;
    for (const auto& file : files) {
        if (file.path.size() > kMaxFolderPathBytes || !PathSanitizer::isSafeRelativePath(file.path)) {
            throw FolderUploadError(400, "Invalid file path in folder upload");
        }
        std::vector<std::string> parts;
        if (!splitStrict(file.path, parts)) {
            throw FolderUploadError(400, "Invalid file path in folder upload");
        }
        if (parts.size() < 2) {
            throw FolderUploadError(400, "Files must come from a single folder");
        }
        if (firstPath) {
            rootName = parts.front();
            firstPath = false;
        } else if (parts.front() != rootName) {
            throw FolderUploadError(400, "Files must come from a single folder");
        }

        // Mục ẩn: thành phần bắt đầu bằng '.'. Khi có override, thành phần gốc bị thay nên không xét.
        bool hidden = false;
        for (std::size_t i = hasOverride ? 1 : 0; i < parts.size(); ++i) {
            if (parts[i].front() == '.') {
                hidden = true;
                break;
            }
        }
        if (hidden) {
            ++skippedHidden;
            continue;
        }
        parts.erase(parts.begin());
        kept.push_back({std::move(parts), file.content});
    }
    if (kept.empty()) {
        throw FolderUploadError(400, "No usable files in folder (hidden files are skipped)");
    }

    // Luật 5: trùng đường dẫn không phân biệt hoa/thường; file cạnh thư mục cùng tên.
    std::set<std::string> filePaths;
    for (const auto& entry : kept) {
        if (!filePaths.insert(lowerAscii(joinParts(entry.parts, entry.parts.size()))).second) {
            throw FolderUploadError(400, "Duplicate file path in folder upload");
        }
    }
    for (const auto& entry : kept) {
        for (std::size_t i = 1; i < entry.parts.size(); ++i) {
            if (filePaths.count(lowerAscii(joinParts(entry.parts, i))) > 0) {
                throw FolderUploadError(400, "A file and a folder share the same path");
            }
        }
    }

    // Luật 6: tạo thư mục gốc mới, không trộn vào thư mục có sẵn.
    const std::string baseName = hasOverride ? nameOverride : rootName;
    std::error_code ec;
    fs::create_directories(downloadDir, ec);
    if (ec) {
        throw FolderUploadError(500, "Failed to prepare download directory");
    }

    fs::path root;
    std::string finalName;
    bool created = false;
    for (int attempt = 0; attempt <= kMaxFolderNameSuffix && !created; ++attempt) {
        finalName = attempt == 0 ? baseName : baseName + " (" + std::to_string(attempt) + ")";
        const fs::path candidate = downloadDir / finalName;
        std::error_code createEc;
        if (fs::create_directory(candidate, createEc)) {
            root = candidate;
            created = true;
        } else if (createEc) {
            std::error_code statEc;
            if (!fs::exists(fs::symlink_status(candidate, statEc))) {
                throw FolderUploadError(500, "Failed to create folder");
            }
            // Đã có một file trùng tên: bỏ qua, thử hậu tố kế tiếp.
        }
    }
    if (!created) {
        throw FolderUploadError(409, "Too many folders with the same name already exist");
    }

    // Luật 7-9: ghi từng file; mọi lỗi thì xóa sạch root vừa tạo.
    StagedFolder staged;
    try {
        std::size_t written = 0;
        for (const auto& entry : kept) {
            fs::path target = root;
            for (const auto& part : entry.parts) {
                target /= part;
            }
            if (!isUnderRoot(root, target)) {
                throw FolderUploadError(400, "Invalid file path in folder upload");
            }
            fs::create_directories(target.parent_path());
            // Không ghi đè: nếu đã có thứ gì ở đó (vd. tên khác hoa/thường trên NTFS) thì dừng.
            if (fs::exists(fs::symlink_status(target))) {
                throw FolderUploadError(400, "Duplicate file path in folder upload");
            }
            {
                std::ofstream output(target, std::ios::binary);
                if (!output) {
                    throw FolderUploadError(500, "Failed to store uploaded file");
                }
                output.write(entry.content.data(), static_cast<std::streamsize>(entry.content.size()));
                output.flush();
                if (!output) {
                    throw FolderUploadError(500, "Failed to store uploaded file");
                }
            }
            staged.totalBytes += entry.content.size();
            ++written;
            if (options.afterFileWritten) {
                options.afterFileWritten(written);
            }
        }
        staged.fileCount = written;
    } catch (const FolderUploadError&) {
        removeQuietly(root);
        throw;
    } catch (const std::exception&) {
        removeQuietly(root);
        throw FolderUploadError(500, "Failed to store uploaded folder");
    }

    staged.root = root;
    staged.name = finalName;
    staged.skippedHidden = skippedHidden;
    return staged;
}

}  // namespace p2p::webui
