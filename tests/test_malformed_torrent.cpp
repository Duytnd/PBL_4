#include "test_framework.h"

#include "core/torrent_file.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace p2p;

namespace {

// Các mẫu .torrent dị dạng được sinh ngay trong test (không phụ thuộc file nhị phân trong repo,
// vì .gitignore chặn *.torrent). Danh sách và lý do dị dạng: tests/samples/malformed/README.md.

struct Sample {
    std::string name;   // tên file khi ghi ra đĩa (để thử TorrentFile::parseFile)
    std::string bytes;  // nội dung bencode thô
};

// Bencode helper: chuỗi "<độ dài>:<nội dung>".
std::string bstr(const std::string& s) { return std::to_string(s.size()) + ":" + s; }
std::string bint(long long v) { return "i" + std::to_string(v) + "e"; }

// Các trường hợp lệ của dictionary info (theo thứ tự khoá tăng dần).
const std::string kLength = bstr("length") + bint(1);
const std::string kName = bstr("name") + bstr("test");
const std::string kPieceLength = bstr("piece length") + bint(262144);
const std::string kPieces = bstr("pieces") + bstr(std::string(20, 'a'));

std::string torrentWithInfo(const std::string& infoBody) {
    return "d" + bstr("info") + "d" + infoBody + "ee";
}

std::string validTorrent() {
    return torrentWithInfo(kLength + kName + kPieceLength + kPieces);
}

std::vector<Sample> syntaxSamples() {
    return {
        {"empty_file.torrent", ""},
        {"unterminated_dict.torrent", "d4:name4:test"},
        {"unterminated_int.torrent", "d4:name3:fooi42"},
        {"truncated_string.torrent", "d4:name100:shorte"},
        {"invalid_int_leading_zeros.torrent", "d4:namei042ee"},
        {"dict_non_string_key.torrent", "di1ei2ee"},
        {"unsupported_token.torrent", "d4:name?e"},
    };
}

std::vector<Sample> schemaSamples() {
    return {
        {"not_a_dict.torrent", bint(42)},
        {"missing_info.torrent", "d" + bstr("announce") + bstr("http://tracker.example:6969/announce") + "e"},
        {"info_not_dict.torrent", "d" + bstr("info") + bstr("not-a-dict") + "e"},
        {"missing_name.torrent", torrentWithInfo(kLength + kPieceLength + kPieces)},
        {"name_not_string.torrent", torrentWithInfo(kLength + bstr("name") + bint(123) + kPieceLength + kPieces)},
        {"missing_piece_length.torrent", torrentWithInfo(kLength + kName + kPieces)},
        {"piece_length_not_int.torrent",
         torrentWithInfo(kLength + kName + bstr("piece length") + bstr("256k") + kPieces)},
        {"zero_piece_length.torrent",
         torrentWithInfo(kLength + kName + bstr("piece length") + bint(0) + kPieces)},
        {"negative_piece_length.torrent",
         torrentWithInfo(kLength + kName + bstr("piece length") + bint(-262144) + kPieces)},
        {"missing_pieces.torrent", torrentWithInfo(kLength + kName + kPieceLength)},
        {"pieces_not_string.torrent", torrentWithInfo(kLength + kName + kPieceLength + bstr("pieces") + bint(123))},
        {"pieces_bad_length.torrent",
         torrentWithInfo(kLength + kName + kPieceLength + bstr("pieces") + bstr(std::string(25, 'a')))},
        {"negative_length.torrent", torrentWithInfo(bstr("length") + bint(-1) + kName + kPieceLength + kPieces)},
        {"piece_count_mismatch.torrent",
         torrentWithInfo(bstr("length") + bint(100) + kName + bstr("piece length") + bint(16384) +
                         bstr("pieces") + bstr(std::string(100, 'a')))},
        {"piece_length_too_large.torrent",
         torrentWithInfo(kLength + kName + bstr("piece length") + bint(1LL << 27) + kPieces)},
        {"zero_total_length.torrent",
         torrentWithInfo(bstr("length") + bint(0) + kName + kPieceLength + bstr("pieces") + bstr(""))},
        {"multi_file_length_overflow.torrent",
         torrentWithInfo(bstr("files") + "l" +
                         "d" + bstr("length") + bint(9223372036854775807LL) + bstr("path") + "l" + bstr("a") + "ee" +
                         "d" + bstr("length") + bint(9223372036854775807LL) + bstr("path") + "l" + bstr("b") + "ee" +
                         "e" + kName + kPieceLength + kPieces)},
    };
}

std::vector<Sample> securitySamples() {
    auto withName = [](const std::string& name) {
        return torrentWithInfo(kLength + bstr("name") + bstr(name) + kPieceLength + kPieces);
    };
    // Multi-file: một phần tử path là "..".
    const std::string traversalEntry = "d" + bstr("length") + bint(1) + bstr("path") + "l" + bstr("..") +
                                       bstr("evil") + "ee";
    auto fileEntry = [](const std::vector<std::string>& parts) {
        std::string path = "l";
        for (const auto& part : parts) path += bstr(part);
        path += "e";
        return "d" + bstr("length") + bint(1) + bstr("path") + path + "e";
    };
    auto multi = [&](const std::vector<std::vector<std::string>>& paths) {
        std::string files = "l";
        for (const auto& parts : paths) files += fileEntry(parts);
        files += "e";
        return torrentWithInfo(bstr("files") + files + kName + kPieceLength + kPieces);
    };
    return {
        {"path_traversal_name.torrent", withName("../../evil.txt")},
        {"path_traversal_absolute.torrent", withName("/etc/passwd")},
        {"path_traversal_windows_drive.torrent", withName("C:/Windows/cmd")},
        {"path_traversal_backslash.torrent", withName("evil\\cmd.exe")},
        {"path_traversal_ads.torrent", withName("file.txt:stream")},
        {"path_traversal_multi_file.torrent",
         torrentWithInfo(bstr("files") + "l" + traversalEntry + "e" + kName + kPieceLength + kPieces)},
        {"device_name_with_extension.torrent", withName("CON.txt")},
        {"device_name_in_file_path.torrent", multi({{"dir", "nul.x"}})},
        {"trailing_dot_name.torrent", withName("evil.")},
        {"trailing_space_file_path.torrent", multi({{"dir", "evil "}})},
        {"forbidden_char_name.torrent", withName("a|b")},
        {"control_char_name.torrent", withName(std::string("a") + '\x01' + "b")},
        {"duplicate_paths_case_insensitive.torrent", multi({{"Readme"}, {"README"}})},
        {"file_and_directory_collision.torrent", multi({{"a"}, {"a", "b"}})},
    };
}

// parse() từ bộ nhớ phải ném exception.
bool parseThrows(const std::string& bytes) {
    try {
        TorrentFile::parse(bytes);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

// parseFile() từ đường dẫn phải ném exception.
bool fileThrows(const std::string& path) {
    try {
        TorrentFile::parseFile(path);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

// Thư mục tạm riêng của test này, dọn khi xong.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / ("p2p_malformed_torrent_" + std::to_string(
#ifdef _WIN32
            _getpid()
#else
            getpid()
#endif
            ));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Mỗi mẫu phải bị từ chối cả khi parse từ bộ nhớ lẫn khi đọc từ file.
void checkAllRejected(const std::vector<Sample>& samples) {
    TempDir dir;
    for (const auto& sample : samples) {
        const auto path = dir.path() / sample.name;
        CHECK(writeFile(path, sample.bytes));
        CHECK(parseThrows(sample.bytes));
        CHECK(fileThrows(path.string()));
    }
}

}  // namespace

void testValidControlTorrentParses() {
    // Đối chứng: torrent gốc dùng để dựng mọi mẫu phải hợp lệ. Nếu không, các test "bị từ chối"
    // bên dưới có thể pass vì lý do sai (mẫu hỏng chứ không phải parser chặn đúng chỗ).
    bool ok = true;
    try {
        const auto meta = TorrentFile::parse(validTorrent());
        CHECK(meta.name == "test");
    } catch (const std::exception&) {
        ok = false;
    }
    CHECK(ok);
}

void testNonExistentFileThrows() {
    CHECK(fileThrows("this_file_does_not_exist_at_all.torrent"));
}

void testSampleSetIsComplete() {
    CHECK(syntaxSamples().size() == 7);
    CHECK(schemaSamples().size() == 17);
    CHECK(securitySamples().size() == 14);
}

void testSyntaxMalformedSamples() {
    checkAllRejected(syntaxSamples());
}

void testSchemaMalformedSamples() {
    checkAllRejected(schemaSamples());
}

void testSecurityMalformedSamples() {
    checkAllRejected(securitySamples());
}

int main() {
    RUN(testValidControlTorrentParses);
    RUN(testNonExistentFileThrows);
    RUN(testSampleSetIsComplete);
    RUN(testSyntaxMalformedSamples);
    RUN(testSchemaMalformedSamples);
    RUN(testSecurityMalformedSamples);
    return TEST_REPORT();
}
