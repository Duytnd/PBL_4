#include "test_framework.h"

#include "core/torrent_file.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace p2p;

namespace {

// Kiểm tra xem việc parse 1 file .torrent từ đường dẫn có ném exception như kỳ vọng không
bool fileThrows(const std::string& path) {
    try {
        TorrentFile::parseFile(path);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

// Tìm đường dẫn tới thư mục tests/samples/malformed
std::string getMalformedDir() {
    // Tìm đường dẫn tương đối từ vị trí chạy test hoặc thư mục gốc dự án
    const std::vector<std::string> candidates = {
        "tests/samples/malformed",
        "../tests/samples/malformed",
        "../../tests/samples/malformed"
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }
    return "";
}

}  // namespace

void testNonExistentFileThrows() {
    CHECK(fileThrows("tests/samples/malformed/this_file_does_not_exist_at_all.torrent"));
}

void testAllMalformedSamplesAreRejected() {
    const std::string malformedDir = getMalformedDir();
    CHECK(!malformedDir.empty());

    // Danh sách 26 file dị dạng bắt buộc phải có và phải bị từ chối
    const std::vector<std::string> expectedFiles = {
        "empty_file.torrent",
        "unterminated_dict.torrent",
        "unterminated_int.torrent",
        "truncated_string.torrent",
        "invalid_int_leading_zeros.torrent",
        "dict_non_string_key.torrent",
        "unsupported_token.torrent",
        "not_a_dict.torrent",
        "missing_info.torrent",
        "info_not_dict.torrent",
        "missing_name.torrent",
        "name_not_string.torrent",
        "missing_piece_length.torrent",
        "piece_length_not_int.torrent",
        "zero_piece_length.torrent",
        "negative_piece_length.torrent",
        "missing_pieces.torrent",
        "pieces_not_string.torrent",
        "pieces_bad_length.torrent",
        "negative_length.torrent",
        "path_traversal_name.torrent",
        "path_traversal_absolute.torrent",
        "path_traversal_windows_drive.torrent",
        "path_traversal_backslash.torrent",
        "path_traversal_ads.torrent",
        "path_traversal_multi_file.torrent"
    };

    for (const auto& fileName : expectedFiles) {
        const std::filesystem::path fullPath = std::filesystem::path(malformedDir) / fileName;
        CHECK(std::filesystem::exists(fullPath));
        CHECK(fileThrows(fullPath.string()));
    }
}

void testSyntaxMalformedSamples() {
    const std::string malformedDir = getMalformedDir();
    CHECK(!malformedDir.empty());

    const std::vector<std::string> syntaxFiles = {
        "empty_file.torrent",
        "unterminated_dict.torrent",
        "unterminated_int.torrent",
        "truncated_string.torrent",
        "invalid_int_leading_zeros.torrent",
        "dict_non_string_key.torrent",
        "unsupported_token.torrent"
    };

    for (const auto& file : syntaxFiles) {
        const auto path = (std::filesystem::path(malformedDir) / file).string();
        CHECK(fileThrows(path));
    }
}

void testSchemaMalformedSamples() {
    const std::string malformedDir = getMalformedDir();
    CHECK(!malformedDir.empty());

    const std::vector<std::string> schemaFiles = {
        "not_a_dict.torrent",
        "missing_info.torrent",
        "info_not_dict.torrent",
        "missing_name.torrent",
        "name_not_string.torrent",
        "missing_piece_length.torrent",
        "piece_length_not_int.torrent",
        "zero_piece_length.torrent",
        "negative_piece_length.torrent",
        "missing_pieces.torrent",
        "pieces_not_string.torrent",
        "pieces_bad_length.torrent",
        "negative_length.torrent"
    };

    for (const auto& file : schemaFiles) {
        const auto path = (std::filesystem::path(malformedDir) / file).string();
        CHECK(fileThrows(path));
    }
}

void testSecurityMalformedSamples() {
    const std::string malformedDir = getMalformedDir();
    CHECK(!malformedDir.empty());

    const std::vector<std::string> securityFiles = {
        "path_traversal_name.torrent",
        "path_traversal_absolute.torrent",
        "path_traversal_windows_drive.torrent",
        "path_traversal_backslash.torrent",
        "path_traversal_ads.torrent",
        "path_traversal_multi_file.torrent"
    };

    for (const auto& file : securityFiles) {
        const auto path = (std::filesystem::path(malformedDir) / file).string();
        CHECK(fileThrows(path));
    }
}

int main() {
    RUN(testNonExistentFileThrows);
    RUN(testAllMalformedSamplesAreRejected);
    RUN(testSyntaxMalformedSamples);
    RUN(testSchemaMalformedSamples);
    RUN(testSecurityMalformedSamples);
    return TEST_REPORT();
}
