// Test tích hợp toàn bộ chuỗi metadata (mục 7, cột mốc giai đoạn 1):
// torrent_builder tạo .torrent -> ghi ra đĩa -> torrent_file đọc lại ->
// piece_manager ghi block từ file gốc và verify đúng hash đã khai báo.

#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/piece_manager.h"
#include "core/torrent_builder.h"
#include "core/torrent_file.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace p2p;

namespace {

std::filesystem::path makeTempDir(const std::string& label) {
    const auto base = std::filesystem::temp_directory_path() /
        ("p2p_integration_" + label + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

void testFullRoundTripSingleFile() {
    const auto dir = makeTempDir("roundtrip");
    const auto sourceFile = dir / "payload.bin";

    // Dữ liệu không chia hết piece length để kiểm tra piece cuối ngắn hơn.
    std::string content;
    for (int i = 0; i < 1000; ++i) {
        content.push_back(static_cast<char>('a' + (i % 26)));
    }
    {
        std::ofstream out(sourceFile, std::ios::binary);
        out << content;
    }

    constexpr std::size_t kPieceLength = 256;
    const std::string torrentBytes = TorrentBuilder::buildTorrent(
        sourceFile, "http://tracker.example:6969/announce", kPieceLength);

    const auto torrentPath = dir / "payload.torrent";
    {
        std::ofstream out(torrentPath, std::ios::binary);
        out.write(torrentBytes.data(), static_cast<std::streamsize>(torrentBytes.size()));
    }

    // Đọc lại .torrent từ đĩa như một client thật sẽ làm.
    const TorrentFileMetadata metadata = TorrentFile::parseFile(torrentPath.string());

    CHECK_EQ(metadata.name, std::string("payload.bin"));
    CHECK_EQ(metadata.pieceLength, static_cast<std::int64_t>(kPieceLength));
    CHECK_EQ(metadata.length, static_cast<std::uint64_t>(content.size()));

    const std::size_t expectedPieceCount = (content.size() + kPieceLength - 1) / kPieceLength;
    CHECK_EQ(metadata.pieces.size(), expectedPieceCount);

    // info_hash tính từ info dict gốc (builder) phải khớp với info_hash tính
    // lại từ infoRaw mà parser giữ được (mục 10: info_hash phải tính từ đúng
    // chuỗi byte gốc của info dict).
    const std::string infoHashFromBuilder = TorrentBuilder::computeInfoHash(sourceFile, kPieceLength);
    const std::string infoHashFromParsedRaw = computeInfoHash(metadata.infoRaw);
    CHECK_EQ(infoHashFromBuilder, infoHashFromParsedRaw);

    // Mô phỏng downloader: đọc từng piece từ file gốc, ghi vào PieceManager
    // bằng piece hash lấy từ .torrent đã parse, verify phải đúng cho mọi piece.
    PieceManager pieceManager(metadata.pieces.size(), kPieceLength, metadata.length, metadata.pieces);

    for (std::size_t pieceIndex = 0; pieceIndex < metadata.pieces.size(); ++pieceIndex) {
        const std::size_t offset = pieceManager.pieceOffset(pieceIndex);
        const std::size_t size = pieceManager.pieceSize(pieceIndex);
        const std::string pieceBytes = content.substr(offset, size);

        CHECK(pieceManager.writeBlock(pieceIndex, 0, size, pieceBytes));
        CHECK(pieceManager.isPieceComplete(pieceIndex));
        CHECK(pieceManager.verifyPiece(pieceIndex));
    }

    CHECK_EQ(pieceManager.assembleFile(), content);

    const auto outputFile = dir / "downloaded.bin";
    pieceManager.writeToFile(outputFile);
    CHECK_EQ(readFile(outputFile), content);

    std::filesystem::remove_all(dir);
}

void testCorruptedPieceFailsVerification() {
    const auto dir = makeTempDir("corrupt");
    const auto sourceFile = dir / "payload.bin";
    {
        std::ofstream out(sourceFile, std::ios::binary);
        out << "0123456789ABCDEF";  // 16 byte, 1 piece với pieceLength 16
    }

    constexpr std::size_t kPieceLength = 16;
    const std::string torrentBytes = TorrentBuilder::buildTorrent(sourceFile, "http://x/announce", kPieceLength);
    const TorrentFileMetadata metadata = TorrentFile::parse(torrentBytes);

    PieceManager pieceManager(metadata.pieces.size(), kPieceLength, metadata.length, metadata.pieces);

    // Giả lập peer gửi sai dữ liệu (bit lỗi) cho piece 0.
    CHECK(pieceManager.writeBlock(0, 0, 16, "XXXXXXXXXXXXXXXX"));
    CHECK(pieceManager.isPieceComplete(0));
    CHECK(!pieceManager.verifyPiece(0));  // phải phát hiện piece sai hash

    std::filesystem::remove_all(dir);
}

int main() {
    RUN(testFullRoundTripSingleFile);
    RUN(testCorruptedPieceFailsVerification);
    return TEST_REPORT();
}
