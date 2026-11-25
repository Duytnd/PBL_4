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
    CHECK(metadata.isMultiFile());
    CHECK_EQ(metadata.files[0].length, std::uint64_t(5));
    CHECK_EQ(metadata.files[1].length, std::uint64_t(7));
    CHECK_EQ(metadata.length, std::uint64_t(12));  // tổng các file
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

void testInfoRawKeepsOriginalBytesForUnsortedInfo() {
    // info dict có khóa KHÔNG theo thứ tự tăng dần ("name" trước "length"):
    // infoRaw phải là đúng đoạn byte gốc, không phải bản mã hóa lại.
    const std::string info =
        "d4:name1:a6:lengthi10e12:piece lengthi16384e6:pieces20:xxxxxxxxxxxxxxxxxxxxe";
    const std::string data = "d8:announce3:foo4:info" + info + "e";
    const auto metadata = TorrentFile::parse(data);
    CHECK_EQ(metadata.infoRaw, info);
    CHECK_EQ(metadata.name, std::string("a"));

    // Khóa root xuất hiện sau "info" cũng không làm lệch đoạn byte.
    const std::string data2 = "d4:info" + info + "8:announce3:fooe";
    CHECK_EQ(TorrentFile::parse(data2).infoRaw, info);
}

void testParseRejectsDuplicateInfoKey() {
    const std::string info =
        "d6:lengthi10e4:name1:a12:piece lengthi16384e6:pieces20:xxxxxxxxxxxxxxxxxxxxe";
    CHECK_THROWS(TorrentFile::parse("d4:info" + info + "4:info" + info + "e"));
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

void testParseValidatesPieceCountAgainstLength() {
    // length=100, piece length=16384 => đúng 1 piece; 5 hash là sai.
    BencodeDict info = makeSingleFileInfo();
    info["piece length"] = BencodeValue(int64_t(16384));
    info["length"] = BencodeValue(int64_t(100));
    info["pieces"] = BencodeValue(std::string(100, 'x'));  // 5 hash
    CHECK(parseThrows(info));

    // Thiếu hash (cần 2 piece nhưng chỉ có 1).
    BencodeDict fewer = makeSingleFileInfo();
    fewer["piece length"] = BencodeValue(int64_t(16384));
    fewer["length"] = BencodeValue(int64_t(16385));
    CHECK(parseThrows(fewer));

    // Đúng số piece thì parse được, kể cả ranh giới chia hết.
    BencodeDict exact = makeSingleFileInfo();
    exact["piece length"] = BencodeValue(int64_t(16384));
    exact["length"] = BencodeValue(int64_t(16384));
    CHECK(!parseThrows(exact));
    exact["length"] = BencodeValue(int64_t(16385));
    exact["pieces"] = BencodeValue(std::string(40, 'x'));
    CHECK(!parseThrows(exact));
}

void testParseRejectsHugePieceLengthAndZeroTotal() {
    BencodeDict huge = makeSingleFileInfo();
    huge["piece length"] = BencodeValue(int64_t(1) << 27);
    CHECK(parseThrows(huge));

    BencodeDict maxOk = makeSingleFileInfo();
    maxOk["piece length"] = BencodeValue(int64_t(1) << 26);
    CHECK(!parseThrows(maxOk));

    BencodeDict zero = makeSingleFileInfo();
    zero["length"] = BencodeValue(int64_t(0));
    zero["pieces"] = BencodeValue(std::string());
    CHECK(parseThrows(zero));

    BencodeDict noLength = makeSingleFileInfo();
    noLength.erase("length");
    CHECK(parseThrows(noLength));
}

void testParseRejectsMultiFileLengthOverflow() {
    BencodeList files;
    for (int i = 0; i < 2; ++i) {
        BencodeDict file;
        file["length"] = BencodeValue(INT64_MAX);
        BencodeList path;
        path.push_back(BencodeValue(std::string(1, static_cast<char>('a' + i))));
        file["path"] = BencodeValue(path);
        files.push_back(BencodeValue(file));
    }
    BencodeDict info = makeSingleFileInfo();
    info.erase("length");
    info["piece length"] = BencodeValue(int64_t(1) << 26);
    info["files"] = BencodeValue(files);
    CHECK(parseThrows(info));
}

namespace {
// Torrent nhiều file từ danh sách đường dẫn (mỗi đường dẫn là list các đoạn).
BencodeDict makeMultiFileInfo(const std::vector<std::vector<std::string>>& paths) {
    BencodeList files;
    for (const auto& parts : paths) {
        BencodeDict file;
        file["length"] = BencodeValue(int64_t(5));
        BencodeList path;
        for (const auto& part : parts) {
            path.push_back(BencodeValue(part));
        }
        file["path"] = BencodeValue(path);
        files.push_back(BencodeValue(file));
    }
    BencodeDict info = makeSingleFileInfo();
    info.erase("length");
    info["files"] = BencodeValue(files);
    return info;
}
}  // namespace

void testParseRejectsCollidingMultiFilePaths() {
    CHECK(!parseThrows(makeMultiFileInfo({{"a"}, {"b"}})));
    CHECK(!parseThrows(makeMultiFileInfo({{"a", "x"}, {"a", "y"}})));
    // Trùng hệt và trùng không phân biệt hoa/thường.
    CHECK(parseThrows(makeMultiFileInfo({{"a"}, {"a"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"a"}, {"A"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"dir", "File.TXT"}, {"DIR", "file.txt"}})));
    // Một đường dẫn là tiền tố thư mục của đường dẫn khác (file "a" cạnh "a/b").
    CHECK(parseThrows(makeMultiFileInfo({{"a"}, {"a", "b"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"a", "b"}, {"a"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"x", "A"}, {"x", "a", "c"}})));
    // "a" và "ab/c" không phải tiền tố thư mục của nhau.
    CHECK(!parseThrows(makeMultiFileInfo({{"a"}, {"ab", "c"}})));
}

void testParseRejectsWindowsSpecialNamesInFilePath() {
    CHECK(parseThrows(makeMultiFileInfo({{"dir", "CON.txt"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"nul.x"}})));
    CHECK(parseThrows(makeMultiFileInfo({{"dir", "name."}})));
    CHECK(parseThrows(makeMultiFileInfo({{"dir", "na?me"}})));
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
    RUN(testInfoRawKeepsOriginalBytesForUnsortedInfo);
    RUN(testParseRejectsDuplicateInfoKey);
    RUN(testParseValidatesPieceCountAgainstLength);
    RUN(testParseRejectsHugePieceLengthAndZeroTotal);
    RUN(testParseRejectsMultiFileLengthOverflow);
    RUN(testParseRejectsCollidingMultiFilePaths);
    RUN(testParseRejectsWindowsSpecialNamesInFilePath);
    return TEST_REPORT();
}
