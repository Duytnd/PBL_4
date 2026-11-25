#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/torrent_builder.h"
#include "core/torrent_file.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace p2p;

namespace {

std::filesystem::path makeTempDir(const std::string& label) {
    const auto base = std::filesystem::temp_directory_path() /
        ("p2p_test_" + label + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

void testSingleFileInfoHashMatchesManualComputation() {
    const auto dir = makeTempDir("single");
    const auto filePath = dir / "data.bin";
    const std::string content(40000, 'A');  // không chia hết cho pieceLength 16384
    writeFile(filePath, content);

    const std::string infoHash = TorrentBuilder::computeInfoHash(filePath, 16384);

    // Tự tính lại bằng tay: chia payload thành piece 256 byte, hash từng piece,
    // ghép lại info dict, rồi SHA-1 hex của info dict đó.
    std::string pieces;
    for (std::size_t offset = 0; offset < content.size(); offset += 16384) {
        const std::size_t chunkSize = std::min<std::size_t>(16384, content.size() - offset);
        pieces += sha1Digest(content.substr(offset, chunkSize));
    }

    BencodeDict info;
    info["name"] = BencodeValue(std::string("data.bin"));
    info["piece length"] = BencodeValue(int64_t(16384));
    info["pieces"] = BencodeValue(pieces);
    info["length"] = BencodeValue(int64_t(content.size()));

    CHECK_EQ(infoHash, computeInfoHash(info));

    std::filesystem::remove_all(dir);
}

void testBuildTorrentProducesValidBencodeWithAnnounce() {
    const auto dir = makeTempDir("announce");
    const auto filePath = dir / "note.txt";
    writeFile(filePath, "hello world");

    const std::string torrentBytes = TorrentBuilder::buildTorrent(filePath, "http://tracker.example:6969/announce", 262144);
    const auto decoded = decodeBencode(torrentBytes);
    const auto* root = decoded.get_if<BencodeDict>();
    CHECK(root != nullptr);
    CHECK(root->count("announce") == 1);
    CHECK(root->count("info") == 1);

    std::filesystem::remove_all(dir);
}

void testMultiFileDirectoryIncludesAllFilesSorted() {
    const auto dir = makeTempDir("multi");
    writeFile(dir / "b.txt", "second");
    writeFile(dir / "a.txt", "first");
    writeFile(dir / "sub" / "c.txt", "nested");

    const BencodeDict info = TorrentBuilder::buildInfoDict(dir, 262144);
    const auto filesIt = info.find("files");
    CHECK(filesIt != info.end());

    const auto* filesList = filesIt->second.get_if<BencodeList>();
    CHECK(filesList != nullptr);
    CHECK_EQ(filesList->size(), std::size_t(3));

    // Phải sắp theo thứ tự byte của đường dẫn tương đối: a.txt, b.txt, sub/c.txt.
    const auto firstPath = (*filesList)[0].get_if<BencodeDict>()->at("path").get_if<BencodeList>();
    CHECK_EQ(firstPath->at(0).get_if<std::string>() != nullptr ? *firstPath->at(0).get_if<std::string>() : "", std::string("a.txt"));

    std::filesystem::remove_all(dir);
}

void testEmptyFileContributesNoBytesButIsListed() {
    const auto dir = makeTempDir("emptyfile");
    writeFile(dir / "empty.txt", "");
    writeFile(dir / "data.txt", "content");

    const BencodeDict info = TorrentBuilder::buildInfoDict(dir, 262144);
    const auto* filesList = info.at("files").get_if<BencodeList>();
    CHECK(filesList != nullptr);
    CHECK_EQ(filesList->size(), std::size_t(2));

    // Multi-file: không có "length" ở cấp info (BEP 3); tổng = tổng các file.
    CHECK(info.count("length") == 0);
    std::int64_t totalLength = 0;
    for (const auto& entry : *filesList) {
        totalLength += *entry.get_if<BencodeDict>()->at("length").get_if<int64_t>();
    }
    CHECK_EQ(totalLength, int64_t(7));  // chỉ "content" góp byte, empty.txt không góp

    std::filesystem::remove_all(dir);
}

void testInvalidPieceLengthFallsBackToDefault() {
    const auto dir = makeTempDir("pl");
    writeFile(dir / "f.bin", "x");

    // 100000 không phải luỹ thừa của 2 -> phải rơi về mặc định 262144 (mục 5.1).
    const BencodeDict info = TorrentBuilder::buildInfoDict(dir / "f.bin", 100000);
    const auto* pieceLength = info.at("piece length").get_if<int64_t>();
    CHECK(pieceLength != nullptr);
    CHECK_EQ(*pieceLength, int64_t(262144));

    std::filesystem::remove_all(dir);
}

void testNameOverrideIsUsed() {
    const auto dir = makeTempDir("nameoverride");
    writeFile(dir / "f.bin", "x");

    const BencodeDict info = TorrentBuilder::buildInfoDict(dir / "f.bin", 262144, "custom-name");
    const auto* name = info.at("name").get_if<std::string>();
    CHECK(name != nullptr);
    CHECK_EQ(*name, std::string("custom-name"));

    std::filesystem::remove_all(dir);
}

void testRejectsNonExistentPath() {
    CHECK_THROWS(TorrentBuilder::buildInfoDict("Z:/this/path/does/not/exist", 262144));
}

void testMultiFileHasFilesButNoLength() {
    const auto dir = makeTempDir("multi_nolength");
    writeFile(dir / "a.txt", "first");
    writeFile(dir / "b.txt", "second");
    writeFile(dir / "sub" / "c.txt", "nested");

    const BencodeDict info = TorrentBuilder::buildInfoDict(dir, 16384);
    CHECK(info.count("files") == 1);
    CHECK(info.count("length") == 0);

    // Round-trip qua parser: tên, tổng length và đường dẫn khớp.
    const std::string torrent = TorrentBuilder::buildTorrent(dir, "http://t/announce", 16384);
    const auto meta = TorrentFile::parse(torrent);
    CHECK_EQ(meta.name, dir.filename().string());
    CHECK_EQ(meta.length, std::uint64_t(5 + 6 + 6));
    CHECK_EQ(meta.files.size(), std::size_t(3));
    CHECK_EQ(meta.filePaths[0], std::string("a.txt"));
    CHECK_EQ(meta.filePaths[1], std::string("b.txt"));
    CHECK_EQ(meta.filePaths[2], std::string("sub/c.txt"));

    // infoRaw của parser phải khớp byte với info vừa dựng.
    CHECK_EQ(meta.infoRaw, encodeBencode(info));

    std::filesystem::remove_all(dir);
}

void testSingleFileHasLengthButNoFiles() {
    const auto dir = makeTempDir("single_nofiles");
    writeFile(dir / "one.bin", "payload");

    const BencodeDict info = TorrentBuilder::buildInfoDict(dir / "one.bin", 16384);
    CHECK(info.count("length") == 1);
    CHECK(info.count("files") == 0);

    const auto meta = TorrentFile::parse(TorrentBuilder::buildTorrent(dir / "one.bin", "", 16384));
    CHECK_EQ(meta.name, std::string("one.bin"));
    CHECK_EQ(meta.length, std::uint64_t(7));
    CHECK(!meta.isMultiFile());

    std::filesystem::remove_all(dir);
}

void testTrailingSeparatorSourcePath() {
    const auto dir = makeTempDir("trailing_sep");
    writeFile(dir / "x.txt", "abc");
    writeFile(dir / "y.txt", "def");

    const std::string withSeparator = dir.string() + "/";
    const BencodeDict info = TorrentBuilder::buildInfoDict(withSeparator, 16384);
    const auto* name = info.at("name").get_if<std::string>();
    CHECK(name != nullptr);
    CHECK(name != nullptr && !name->empty());
    CHECK_EQ(name != nullptr ? *name : std::string(), dir.filename().string());

    std::filesystem::remove_all(dir);
}

void testSmallPieceLengthIsRaisedToMinimum() {
    const auto dir = makeTempDir("small_piece");
    writeFile(dir / "f.bin", std::string(40000, 'z'));

    for (std::size_t requested : {std::size_t(1), std::size_t(256), std::size_t(8192)}) {
        const BencodeDict info = TorrentBuilder::buildInfoDict(dir / "f.bin", requested);
        CHECK_EQ(*info.at("piece length").get_if<int64_t>(), int64_t(16384));
        CHECK_EQ(info.at("pieces").get_if<std::string>()->size(), std::size_t(3 * 20));
    }
    // Từ 16384 trở lên và là luỹ thừa 2 thì giữ nguyên.
    const BencodeDict kept = TorrentBuilder::buildInfoDict(dir / "f.bin", 32768);
    CHECK_EQ(*kept.at("piece length").get_if<int64_t>(), int64_t(32768));

    std::filesystem::remove_all(dir);
}

#ifdef _WIN32
void testUnreadableFileFailsInsteadOfSkipping() {
    const auto dir = makeTempDir("unreadable");
    writeFile(dir / "a.txt", "first");
    writeFile(dir / "b.txt", "second");

    // Khoá độc quyền b.txt để ifstream không mở được.
    const auto locked = (dir / "b.txt").string();
    HANDLE handle = CreateFileA(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);

    bool threwRuntimeError = false;
    try {
        TorrentBuilder::buildInfoDict(dir, 16384);
    } catch (const std::runtime_error&) {
        threwRuntimeError = true;
    }
    CHECK(threwRuntimeError);

    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    std::filesystem::remove_all(dir);
}
#endif

int main() {
    RUN(testSingleFileInfoHashMatchesManualComputation);
    RUN(testBuildTorrentProducesValidBencodeWithAnnounce);
    RUN(testMultiFileDirectoryIncludesAllFilesSorted);
    RUN(testEmptyFileContributesNoBytesButIsListed);
    RUN(testInvalidPieceLengthFallsBackToDefault);
    RUN(testNameOverrideIsUsed);
    RUN(testRejectsNonExistentPath);
    RUN(testMultiFileHasFilesButNoLength);
    RUN(testSingleFileHasLengthButNoFiles);
    RUN(testTrailingSeparatorSourcePath);
    RUN(testSmallPieceLengthIsRaisedToMinimum);
#ifdef _WIN32
    RUN(testUnreadableFileFailsInsteadOfSkipping);
#endif
    return TEST_REPORT();
}
