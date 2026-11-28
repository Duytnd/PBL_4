#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "webui/folder_upload.h"

using namespace p2p::webui;
namespace fs = std::filesystem;

namespace {

// Thư mục tạm riêng nằm ngoài cây dự án.
fs::path makeTempDir(const std::string& tag) {
    static int counter = 0;
    fs::path dir = fs::temp_directory_path() /
                   ("p2p_folder_upload_" + tag + "_" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void cleanup(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Trả về mã lỗi HTTP, hoặc 0 nếu không ném lỗi.
int stageStatus(const fs::path& dir, const std::vector<UploadedFile>& files,
                const std::string& name = "", const FolderUploadOptions& options = {}) {
    try {
        stageFolderUpload(dir, files, name, options);
        return 0;
    } catch (const FolderUploadError& error) {
        return error.httpStatus;
    }
}

std::size_t entryCount(const fs::path& dir) {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) {
        ++n;
    }
    return n;
}

}  // namespace

void testStagesNestedFolder() {
    const fs::path dir = makeTempDir("nested");
    std::vector<UploadedFile> files = {
        {"album/a.txt", "hello"},
        {"album/sub/b.bin", std::string_view("12\0 34", 5)},
        {"album/sub/deep/c.txt", ""},
    };
    const StagedFolder staged = stageFolderUpload(dir, files, "");
    CHECK_EQ(staged.name, std::string("album"));
    CHECK_EQ(staged.fileCount, std::size_t(3));
    CHECK_EQ(staged.totalBytes, std::uint64_t(10));
    CHECK_EQ(staged.skippedHidden, std::size_t(0));
    CHECK(staged.root == dir / "album");
    CHECK_EQ(readAll(dir / "album" / "a.txt"), std::string("hello"));
    CHECK_EQ(readAll(dir / "album" / "sub" / "b.bin"), std::string("12\0 34", 5));
    CHECK(fs::exists(dir / "album" / "sub" / "deep" / "c.txt"));
    CHECK_EQ(fs::file_size(dir / "album" / "sub" / "deep" / "c.txt"), std::uintmax_t(0));
    CHECK_EQ(entryCount(dir), std::size_t(1));
    cleanup(dir);
}

void testRejectsEmptyAndLimits() {
    const fs::path dir = makeTempDir("limits");
    CHECK_EQ(stageStatus(dir, {}), 400);

    FolderUploadOptions small;
    small.maxFiles = 2;
    small.maxBytes = 10;
    std::vector<UploadedFile> three = {{"f/a", "1"}, {"f/b", "2"}, {"f/c", "3"}};
    CHECK_EQ(stageStatus(dir, three, "", small), 413);
    std::vector<UploadedFile> big = {{"f/a", "123456"}, {"f/b", "12345"}};
    CHECK_EQ(stageStatus(dir, big, "", small), 413);
    // Đúng bằng giới hạn thì được.
    std::vector<UploadedFile> exact = {{"f/a", "12345"}, {"f/b", "12345"}};
    CHECK_EQ(stageStatus(dir, exact, "", small), 0);
    // Bị từ chối thì không để lại gì (ngoài thư mục f của lần hợp lệ).
    CHECK_EQ(entryCount(dir), std::size_t(1));

    // Hằng thật khớp spec.
    CHECK_EQ(kMaxFolderFiles, std::size_t(10000));
    CHECK_EQ(kMaxFolderBytes, std::uint64_t(256ull * 1024 * 1024));
    cleanup(dir);
}

void testRejectsUnsafePaths() {
    const fs::path dir = makeTempDir("unsafe");
    const std::vector<std::string> bad = {
        "f/../x.txt",   "../f/x.txt",     "/f/x.txt",       "f\\x.txt",     "C:/f/x.txt",
        "f/C:x.txt",    "f/CON.txt",      "f/nul",          "f/a?.txt",     "f/a.",
        "f//x.txt",     std::string("f/a\0b", 5),            "f/./x.txt",    "f/x.txt/",
    };
    for (const auto& path : bad) {
        std::vector<UploadedFile> files = {{"f/ok.txt", "1"}, {path, "x"}};
        CHECK_EQ(stageStatus(dir, files), 400);
    }
    CHECK_EQ(stageStatus(dir, {{std::string("f/") + std::string(1030, 'a'), "x"}}), 400);
    CHECK_EQ(entryCount(dir), std::size_t(0));  // không ghi gì khi bị từ chối
    cleanup(dir);
}

void testRequiresSingleFolder() {
    const fs::path dir = makeTempDir("single");
    CHECK_EQ(stageStatus(dir, {{"f/a.txt", "1"}, {"g/b.txt", "2"}}), 400);
    CHECK_EQ(stageStatus(dir, {{"a.txt", "1"}}), 400);                     // một thành phần
    CHECK_EQ(stageStatus(dir, {{"f/a.txt", "1"}, {"b.txt", "2"}}), 400);   // lẫn file ở gốc
    CHECK_EQ(entryCount(dir), std::size_t(0));
    cleanup(dir);
}

void testNameOverride() {
    const fs::path dir = makeTempDir("override");
    const StagedFolder staged = stageFolderUpload(dir, {{"orig/a.txt", "1"}, {"orig/s/b.txt", "2"}}, "renamed");
    CHECK_EQ(staged.name, std::string("renamed"));
    CHECK(fs::exists(dir / "renamed" / "a.txt"));
    CHECK(fs::exists(dir / "renamed" / "s" / "b.txt"));
    CHECK(!fs::exists(dir / "orig"));

    CHECK_EQ(stageStatus(dir, {{"orig/a.txt", "1"}}, "a/b"), 400);
    CHECK_EQ(stageStatus(dir, {{"orig/a.txt", "1"}}, ".."), 400);
    CHECK_EQ(stageStatus(dir, {{"orig/a.txt", "1"}}, "C:x"), 400);
    CHECK_EQ(stageStatus(dir, {{"orig/a.txt", "1"}}, "CON"), 400);
    CHECK_EQ(entryCount(dir), std::size_t(1));
    cleanup(dir);
}

void testSkipsHiddenEntries() {
    const fs::path dir = makeTempDir("hidden");
    std::vector<UploadedFile> files = {
        {"proj/a.txt", "1"},
        {"proj/.DS_Store", "x"},
        {"proj/.git/config", "x"},
        {"proj/sub/.hidden/inner.txt", "x"},
        {"proj/sub/b.txt", "22"},
    };
    const StagedFolder staged = stageFolderUpload(dir, files, "");
    CHECK_EQ(staged.fileCount, std::size_t(2));
    CHECK_EQ(staged.skippedHidden, std::size_t(3));
    CHECK_EQ(staged.totalBytes, std::uint64_t(3));
    CHECK(!fs::exists(dir / "proj" / ".DS_Store"));
    CHECK(!fs::exists(dir / "proj" / ".git"));
    CHECK(!fs::exists(dir / "proj" / "sub" / ".hidden"));
    CHECK(fs::exists(dir / "proj" / "sub" / "b.txt"));

    // Toàn bộ là file ẩn -> 400 và không tạo thư mục.
    const fs::path dir2 = makeTempDir("hidden_all");
    CHECK_EQ(stageStatus(dir2, {{"p/.a", "1"}, {"p/.git/x", "2"}}), 400);
    CHECK_EQ(entryCount(dir2), std::size_t(0));
    cleanup(dir);
    cleanup(dir2);
}

void testRejectsDuplicatePaths() {
    const fs::path dir = makeTempDir("dup");
    CHECK_EQ(stageStatus(dir, {{"f/a.txt", "1"}, {"f/a.txt", "2"}}), 400);
    CHECK_EQ(stageStatus(dir, {{"f/A.txt", "1"}, {"f/a.TXT", "2"}}), 400);
    CHECK_EQ(stageStatus(dir, {{"f/Sub/x", "1"}, {"f/sub/X", "2"}}), 400);
    // File cạnh thư mục cùng tên (cũng không phân biệt hoa/thường).
    CHECK_EQ(stageStatus(dir, {{"f/a", "1"}, {"f/a/b", "2"}}), 400);
    CHECK_EQ(stageStatus(dir, {{"f/A", "1"}, {"f/a/b/c", "2"}}), 400);
    // Cùng thư mục, khác tên thì ổn.
    CHECK_EQ(stageStatus(dir, {{"f/a", "1"}, {"f/ab/b", "2"}}), 0);
    CHECK_EQ(entryCount(dir), std::size_t(1));  // chỉ lần hợp lệ cuối để lại "f"
    cleanup(dir);
}

void testExistingTargetGetsSuffix() {
    const fs::path dir = makeTempDir("suffix");
    fs::create_directories(dir / "album");
    {
        std::ofstream(dir / "album" / "keep.txt") << "keep";
    }
    const StagedFolder first = stageFolderUpload(dir, {{"album/a.txt", "1"}}, "");
    CHECK_EQ(first.name, std::string("album (1)"));
    CHECK(fs::exists(dir / "album (1)" / "a.txt"));
    CHECK(!fs::exists(dir / "album" / "a.txt"));  // không trộn vào thư mục cũ
    CHECK_EQ(readAll(dir / "album" / "keep.txt"), std::string("keep"));

    const StagedFolder second = stageFolderUpload(dir, {{"album/a.txt", "2"}}, "");
    CHECK_EQ(second.name, std::string("album (2)"));
    CHECK_EQ(readAll(dir / "album (1)" / "a.txt"), std::string("1"));

    // Một FILE trùng tên cũng bị coi là đã tồn tại.
    {
        std::ofstream(dir / "doc") << "i am a file";
    }
    const StagedFolder third = stageFolderUpload(dir, {{"doc/x.txt", "1"}}, "");
    CHECK_EQ(third.name, std::string("doc (1)"));
    CHECK_EQ(readAll(dir / "doc"), std::string("i am a file"));
    cleanup(dir);
}

void testTooManySameNameFolders() {
    const fs::path dir = makeTempDir("many");
    fs::create_directories(dir / "dir");
    for (int i = 1; i <= kMaxFolderNameSuffix; ++i) {
        fs::create_directories(dir / ("dir (" + std::to_string(i) + ")"));
    }
    CHECK_EQ(stageStatus(dir, {{"dir/a.txt", "1"}}), 409);
    CHECK_EQ(entryCount(dir), std::size_t(kMaxFolderNameSuffix + 1));
    // Chỉ cần trống một chỗ là dùng được.
    fs::remove_all(dir / "dir (57)");
    const StagedFolder staged = stageFolderUpload(dir, {{"dir/a.txt", "1"}}, "");
    CHECK_EQ(staged.name, std::string("dir (57)"));
    cleanup(dir);
}

void testWriteFailureCleansUp() {
    const fs::path dir = makeTempDir("fail");
    FolderUploadOptions options;
    options.afterFileWritten = [](std::size_t written) {
        if (written == 2) {
            throw std::runtime_error("disk full");
        }
    };
    std::vector<UploadedFile> files = {{"f/a.txt", "1"}, {"f/s/b.txt", "2"}, {"f/s/c.txt", "3"}};
    CHECK_EQ(stageStatus(dir, files, "", options), 500);
    CHECK_EQ(entryCount(dir), std::size_t(0));  // không còn file mồ côi, kể cả thư mục gốc

    // Thư mục có sẵn từ trước không bị đụng khi lần sau lỗi: root là "f (1)".
    fs::create_directories(dir / "f");
    {
        std::ofstream(dir / "f" / "keep.txt") << "keep";
    }
    CHECK_EQ(stageStatus(dir, files, "", options), 500);
    CHECK(fs::exists(dir / "f" / "keep.txt"));
    CHECK(!fs::exists(dir / "f (1)"));
    CHECK_EQ(entryCount(dir), std::size_t(1));

    // Lỗi FolderUploadError từ hook cũng dọn sạch và giữ nguyên mã.
    options.afterFileWritten = [](std::size_t) { throw FolderUploadError(507, "quota"); };
    CHECK_EQ(stageStatus(dir, files, "", options), 507);
    CHECK_EQ(entryCount(dir), std::size_t(1));
    cleanup(dir);
}

void testNeverWritesOutsideDownloadDir() {
    const fs::path dir = makeTempDir("outside");
    const fs::path downloads = dir / "downloads";
    fs::create_directories(downloads);
    const std::vector<std::string> attacks = {"f/../../evil.txt", "f/..\\..\\evil.txt", "../evil.txt", "f/%2e%2e/x"};
    for (const auto& path : attacks) {
        stageStatus(downloads, {{"f/ok.txt", "1"}, {path, "x"}});
    }
    CHECK(!fs::exists(dir / "evil.txt"));
    CHECK(!fs::exists(dir / "x"));
    // Chỉ "downloads" nằm trong dir, và trong đó không có gì lọt ra.
    CHECK_EQ(entryCount(dir), std::size_t(1));
    cleanup(dir);
}

int main() {
    RUN(testStagesNestedFolder);
    RUN(testRejectsEmptyAndLimits);
    RUN(testRejectsUnsafePaths);
    RUN(testRequiresSingleFolder);
    RUN(testNameOverride);
    RUN(testSkipsHiddenEntries);
    RUN(testRejectsDuplicatePaths);
    RUN(testExistingTargetGetsSuffix);
    RUN(testTooManySameNameFolders);
    RUN(testWriteFailureCleansUp);
    RUN(testNeverWritesOutsideDownloadDir);
    return TEST_REPORT();
}
