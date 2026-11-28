#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace p2p::webui {

// Một file do trình duyệt gửi lên khi người dùng chọn cả thư mục.
// path: đường dẫn tương đối (dùng '/'), phần tử đầu là tên thư mục được chọn.
struct UploadedFile {
    std::string path;
    std::string_view content;
};

struct StagedFolder {
    std::filesystem::path root;  // downloadDir/<tên cuối cùng>, do chính hàm này tạo
    std::string name;            // tên thư mục cuối cùng (có thể mang hậu tố " (n)")
    std::size_t fileCount = 0;
    std::uint64_t totalBytes = 0;
    std::size_t skippedHidden = 0;
};

class FolderUploadError : public std::runtime_error {
public:
    FolderUploadError(int status, const std::string& message)
        : std::runtime_error(message), httpStatus(status) {}
    int httpStatus;
};

inline constexpr std::size_t kMaxFolderFiles = 10000;
inline constexpr std::uint64_t kMaxFolderBytes = 256ull * 1024 * 1024;
inline constexpr std::size_t kMaxFolderPathBytes = 1024;
inline constexpr int kMaxFolderNameSuffix = 100;

// Tham số tùy chọn: mặc định là giới hạn thật; test thu nhỏ giới hạn hoặc tiêm lỗi ghi.
struct FolderUploadOptions {
    std::size_t maxFiles = kMaxFolderFiles;
    std::uint64_t maxBytes = kMaxFolderBytes;
    // Gọi sau khi mỗi file được ghi xong (đối số: số file đã ghi). Ném lỗi để giả lập ghi hỏng.
    std::function<void(std::size_t)> afterFileWritten;
};

// Ghi các file tải lên vào downloadDir/<tên thư mục>. Không bao giờ trộn vào thư mục có sẵn:
// trùng tên thì dùng "<tên> (1)", "<tên> (2)"... Ném FolderUploadError; khi lỗi, thư mục đã tạo bị xóa sạch.
StagedFolder stageFolderUpload(const std::filesystem::path& downloadDir,
                               const std::vector<UploadedFile>& files,
                               const std::string& nameOverride,
                               const FolderUploadOptions& options = {});

}  // namespace p2p::webui
