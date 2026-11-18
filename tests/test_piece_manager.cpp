#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/piece_manager.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace p2p;

void testOffsetsAndSizesWithUnevenLastPiece() {
    // Tổng 10 byte, piece length 4 -> piece 0,1 đủ 4 byte, piece 2 chỉ còn 2 byte.
    PieceManager manager(3, 4, 10);

    CHECK_EQ(manager.pieceCount(), std::size_t(3));
    CHECK_EQ(manager.pieceOffset(0), std::size_t(0));
    CHECK_EQ(manager.pieceOffset(1), std::size_t(4));
    CHECK_EQ(manager.pieceOffset(2), std::size_t(8));
    CHECK_EQ(manager.pieceSize(0), std::size_t(4));
    CHECK_EQ(manager.pieceSize(1), std::size_t(4));
    CHECK_EQ(manager.pieceSize(2), std::size_t(2));
    CHECK_EQ(manager.pieceSize(99), std::size_t(0));  // ngoài phạm vi
}

void testWriteBlockThenCompleteAndVerify() {
    const std::string pieceData = "abcd";
    const std::string hash = sha1Digest(pieceData);
    PieceManager manager(1, 4, 4, {hash});

    CHECK(!manager.isPieceComplete(0));
    CHECK(manager.writeBlock(0, 0, 2, "ab"));
    CHECK(!manager.isPieceComplete(0));  // mới có nửa piece
    CHECK(manager.writeBlock(0, 2, 2, "cd"));
    CHECK(manager.isPieceComplete(0));
    CHECK(manager.verifyPiece(0));
}

void testVerifyFailsOnWrongHash() {
    PieceManager manager(1, 4, 4, {std::string(20, 'z')});  // hash sai hoàn toàn
    CHECK(manager.writeBlock(0, 0, 4, "abcd"));
    CHECK(manager.isPieceComplete(0));
    CHECK(!manager.verifyPiece(0));
}

void testVerifyFailsWithoutExpectedHash() {
    PieceManager manager(1, 4, 4);  // không truyền pieceHashes
    CHECK(manager.writeBlock(0, 0, 4, "abcd"));
    CHECK(!manager.verifyPiece(0));  // không có hash để so sánh -> coi là chưa verify được
}

void testWriteBlockRejectsOutOfRange() {
    PieceManager manager(1, 4, 4);
    CHECK(!manager.writeBlock(5, 0, 4, "abcd"));         // piece index ngoài phạm vi
    CHECK(!manager.writeBlock(0, 0, 100, "abcd"));        // length > pieceLength
    CHECK(!manager.writeBlock(0, 3, 4, "abcd"));          // offset + length > pieceLength
    CHECK(!manager.writeBlock(0, 0, 10, "short"));        // blockData ngắn hơn length khai báo
}

void testWriteBlockRejectsBeyondLastPieceSize() {
    // Tổng 6 byte, pieceLength 4 -> piece 1 chỉ có 2 byte thật, request 4 byte là vượt quá.
    PieceManager manager(2, 4, 6);
    CHECK(!manager.writeBlock(1, 0, 4, "abcd"));
    CHECK(manager.writeBlock(1, 0, 2, "ab"));
    CHECK(manager.isPieceComplete(1));
}

void testAssembleFileConcatenatesInOrder() {
    PieceManager manager(2, 4, 6);
    CHECK(manager.writeBlock(0, 0, 4, "abcd"));
    CHECK(manager.writeBlock(1, 0, 2, "ef"));
    CHECK_EQ(manager.assembleFile(), std::string("abcdef"));
}

void testAssembleFileSkipsIncompletePieces() {
    PieceManager manager(2, 4, 6);
    CHECK(manager.writeBlock(0, 0, 4, "abcd"));
    // Không ghi piece 1 -> assembleFile chỉ ra piece đã có, không crash.
    const std::string assembled = manager.assembleFile();
    CHECK(assembled.size() <= std::size_t(6));
}

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

void testMultiFileRoundTrip() {
    // Luồng 10 byte "0123456789" chia thành a.bin(3) + sub/empty(0) + sub/b.bin(5) + c.bin(2),
    // piece length 4 -> piece 0 vắt qua a.bin/b.bin, piece 1 nằm trong b.bin, piece 2 vắt b.bin/c.bin.
    const std::string payload = "0123456789";
    const std::vector<FileEntry> files = {{"a.bin", 3}, {"sub/empty.bin", 0}, {"sub/b.bin", 5}, {"c.bin", 2}};
    std::vector<std::string> hashes = {sha1Digest(payload.substr(0, 4)), sha1Digest(payload.substr(4, 4)),
                                       sha1Digest(payload.substr(8, 2))};

    const auto root = std::filesystem::temp_directory_path() / "p2p_multifile_test";
    std::filesystem::remove_all(root);

    PieceManager writer(3, 4, 10, hashes);
    CHECK(writer.writeBlock(0, 0, 4, payload.substr(0, 4)));
    CHECK(writer.writeBlock(2, 0, 2, payload.substr(8, 2)));
    CHECK(writer.writePieceToFiles(0, root, files));
    CHECK(writer.writePieceToFiles(2, root, files));
    CHECK(!writer.writePieceToFiles(1, root, files));  // piece 1 chưa đủ byte

    CHECK_EQ(readAll(root / "a.bin").size(), std::size_t(3));
    CHECK_EQ(readAll(root / "a.bin"), std::string("012"));
    CHECK_EQ(readAll(root / "sub/b.bin").size(), std::size_t(5));  // sparse, giãn đủ độ dài
    CHECK_EQ(readAll(root / "c.bin"), std::string("89"));

    // Nạp lại: piece 1 chưa ghi nên sai hash, piece 0 và 2 đúng.
    PieceManager reader(3, 4, 10, hashes);
    CHECK(reader.loadFromFiles(root, files));
    CHECK(reader.verifyPiece(0));
    CHECK(!reader.verifyPiece(1));
    CHECK(reader.verifyPiece(2));

    CHECK(writer.writeBlock(1, 0, 4, payload.substr(4, 4)));
    CHECK(writer.writePieceToFiles(1, root, files));
    CHECK(PieceManager::ensureFiles(root, files));
    CHECK(std::filesystem::exists(root / "sub/empty.bin"));
    CHECK_EQ(readAll(root / "sub/b.bin"), std::string("34567"));

    PieceManager full(3, 4, 10, hashes);
    CHECK(full.loadFromFiles(root, files));
    CHECK(full.verifyPiece(0) && full.verifyPiece(1) && full.verifyPiece(2));
    CHECK_EQ(full.assembleFile(), payload);

    std::filesystem::remove_all(root);
}

void testMultiFileRejectsMismatchedLayoutAndMissingFiles() {
    const auto root = std::filesystem::temp_directory_path() / "p2p_multifile_missing";
    std::filesystem::remove_all(root);
    PieceManager manager(1, 4, 4);
    CHECK(!manager.loadFromFiles(root, {{"x", 4}}));          // không có file nào
    CHECK(!manager.loadFromFiles(root, {{"x", 3}}));          // tổng độ dài sai
    CHECK(!manager.writePieceToFiles(0, root, {{"x", 3}}));
}

int main() {
    RUN(testOffsetsAndSizesWithUnevenLastPiece);
    RUN(testWriteBlockThenCompleteAndVerify);
    RUN(testVerifyFailsOnWrongHash);
    RUN(testVerifyFailsWithoutExpectedHash);
    RUN(testWriteBlockRejectsOutOfRange);
    RUN(testWriteBlockRejectsBeyondLastPieceSize);
    RUN(testAssembleFileConcatenatesInOrder);
    RUN(testAssembleFileSkipsIncompletePieces);
    RUN(testMultiFileRoundTrip);
    RUN(testMultiFileRejectsMismatchedLayoutAndMissingFiles);
    return TEST_REPORT();
}
