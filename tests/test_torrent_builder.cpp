#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/torrent_builder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
    const std::string content(1000, 'A');  // không chia hết cho pieceLength 256
    writeFile(filePath, content);

    const std::string infoHash = TorrentBuilder::computeInfoHash(filePath, 256);

    // Tự tính lại bằng tay: chia payload thành piece 256 byte, hash từng piece,
    // ghép lại info dict, rồi SHA-1 hex của info dict đó.
    std::string pieces;
    for (std::size_t offset = 0; offset < content.size(); offset += 256) {
        const std::size_t chunkSize = std::min<std::size_t>(256, content.size() - offset);
        pieces += sha1Digest(content.substr(offset, chunkSize));
    }

    BencodeDict info;
    info["name"] = BencodeValue(std::string("data.bin"));
    info["piece length"] = BencodeValue(int64_t(256));
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

    const auto totalLength = info.at("length").get_if<int64_t>();
    CHECK(totalLength != nullptr);
    CHECK_EQ(*totalLength, int64_t(7));  // chỉ "content" góp byte, empty.txt không góp

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

int main() {
    RUN(testSingleFileInfoHashMatchesManualComputation);
    RUN(testBuildTorrentProducesValidBencodeWithAnnounce);
    RUN(testMultiFileDirectoryIncludesAllFilesSorted);
    RUN(testEmptyFileContributesNoBytesButIsListed);
    RUN(testInvalidPieceLengthFallsBackToDefault);
    RUN(testNameOverrideIsUsed);
    RUN(testRejectsNonExistentPath);
    return TEST_REPORT();
}
