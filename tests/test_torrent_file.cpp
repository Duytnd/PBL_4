#include "test_framework.h"

#include "core/torrent_file.h"

using namespace p2p;

namespace {

// Dựng info dict đơn giản 1 file, 1 piece, cho các test dưới.
BencodeDict makeSingleFileInfo() {
    BencodeDict info;
    info["name"] = BencodeValue(std::string("example.txt"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["length"] = BencodeValue(int64_t(10));
    info["pieces"] = BencodeValue(std::string(20, 'x'));  // giả hash, đủ 20 byte
    return info;
}

}  // namespace

void testParseSingleFileTorrent() {
    BencodeDict root;
    root["announce"] = BencodeValue(std::string("http://tracker.example:6969/announce"));
    root["info"] = BencodeValue(makeSingleFileInfo());

    const auto metadata = TorrentFile::parse(encodeBencode(root));

    CHECK_EQ(metadata.announce, std::string("http://tracker.example:6969/announce"));
    CHECK_EQ(metadata.name, std::string("example.txt"));
    CHECK_EQ(metadata.pieceLength, int64_t(262144));
    CHECK_EQ(metadata.length, std::uint64_t(10));
    CHECK_EQ(metadata.pieces.size(), std::size_t(1));
    CHECK(metadata.filePaths.empty());
    CHECK(metadata.hasInfo());
}

void testParseMultiFileTorrent() {
    BencodeDict fileA;
    fileA["length"] = BencodeValue(int64_t(5));
    BencodeList pathA;
    pathA.emplace_back(std::string("dir"));
    pathA.emplace_back(std::string("a.txt"));
    fileA["path"] = BencodeValue(pathA);

    BencodeDict fileB;
    fileB["length"] = BencodeValue(int64_t(7));
    BencodeList pathB;
    pathB.emplace_back(std::string("b.txt"));
    fileB["path"] = BencodeValue(pathB);

    BencodeList files;
    files.emplace_back(BencodeValue(fileA));
    files.emplace_back(BencodeValue(fileB));

    BencodeDict info;
    info["name"] = BencodeValue(std::string("root"));
    info["piece length"] = BencodeValue(int64_t(262144));
    info["pieces"] = BencodeValue(std::string(20, 'y'));
    info["files"] = BencodeValue(files);

    BencodeDict root;
    root["announce"] = BencodeValue(std::string("http://tracker.example:6969/announce"));
    root["info"] = BencodeValue(info);

    const auto metadata = TorrentFile::parse(encodeBencode(root));

    CHECK_EQ(metadata.filePaths.size(), std::size_t(2));
    CHECK_EQ(metadata.filePaths[0], std::string("dir/a.txt"));
    CHECK_EQ(metadata.filePaths[1], std::string("b.txt"));
}

void testParseRejectsMissingInfo() {
    BencodeDict root;
    root["announce"] = BencodeValue(std::string("http://tracker.example:6969/announce"));
    CHECK_THROWS(TorrentFile::parse(encodeBencode(root)));
}

void testParseRejectsPiecesNotMultipleOf20() {
    BencodeDict info = makeSingleFileInfo();
    info["pieces"] = BencodeValue(std::string(21, 'x'));  // không chia hết cho 20
    BencodeDict root;
    root["info"] = BencodeValue(info);
    CHECK_THROWS(TorrentFile::parse(encodeBencode(root)));
}

void testParseRejectsMissingName() {
    BencodeDict info = makeSingleFileInfo();
    info.erase("name");
    BencodeDict root;
    root["info"] = BencodeValue(info);
    CHECK_THROWS(TorrentFile::parse(encodeBencode(root)));
}

void testParseRejectsNonDictionaryTopLevel() {
    CHECK_THROWS(TorrentFile::parse(encodeBencode(int64_t(42))));
}

void testInfoRawMatchesOriginalEncodedInfo() {
    // info_hash phải tính từ đúng chuỗi byte gốc của info dict (mục 10).
    const BencodeDict info = makeSingleFileInfo();
    BencodeDict root;
    root["info"] = BencodeValue(info);

    const auto metadata = TorrentFile::parse(encodeBencode(root));
    CHECK_EQ(metadata.infoRaw, encodeBencode(info));
}

namespace {
bool parseThrows(const BencodeDict& info) {
    BencodeDict root;
    root["info"] = BencodeValue(info);
    try {
        TorrentFile::parse(encodeBencode(root));
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
}  // namespace

void testParseRejectsUnsafeName() {
    for (const char* bad : {"../../evil", "/etc/passwd", "C:/Windows/x", "a\\b", "file.txt:stream"}) {
        BencodeDict info = makeSingleFileInfo();
        info["name"] = BencodeValue(std::string(bad));
        CHECK(parseThrows(info));
    }
}

void testParseRejectsUnsafeFilePath() {
    BencodeDict info = makeSingleFileInfo();
    BencodeDict file;
    file["length"] = BencodeValue(int64_t(5));
    BencodeList path;
    path.push_back(BencodeValue(std::string("..")));
    path.push_back(BencodeValue(std::string("x")));
    file["path"] = BencodeValue(path);
    BencodeList files;
    files.push_back(BencodeValue(file));
    info["files"] = BencodeValue(files);
    CHECK(parseThrows(info));
}

void testParseRejectsBadPieceLengthAndLength() {
    BencodeDict zero = makeSingleFileInfo();
    zero["piece length"] = BencodeValue(int64_t(0));
    CHECK(parseThrows(zero));

    BencodeDict negative = makeSingleFileInfo();
    negative["length"] = BencodeValue(int64_t(-1));
    CHECK(parseThrows(negative));
}

int main() {
    RUN(testParseRejectsUnsafeName);
    RUN(testParseRejectsUnsafeFilePath);
    RUN(testParseRejectsBadPieceLengthAndLength);
    RUN(testParseSingleFileTorrent);
    RUN(testParseMultiFileTorrent);
    RUN(testParseRejectsMissingInfo);
    RUN(testParseRejectsPiecesNotMultipleOf20);
    RUN(testParseRejectsMissingName);
    RUN(testParseRejectsNonDictionaryTopLevel);
    RUN(testInfoRawMatchesOriginalEncodedInfo);
    return TEST_REPORT();
}
