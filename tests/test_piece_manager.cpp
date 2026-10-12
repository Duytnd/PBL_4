#include "test_framework.h"

#include "core/hash_utils.h"
#include "core/piece_manager.h"

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

int main() {
    RUN(testOffsetsAndSizesWithUnevenLastPiece);
    RUN(testWriteBlockThenCompleteAndVerify);
    RUN(testVerifyFailsOnWrongHash);
    RUN(testVerifyFailsWithoutExpectedHash);
    RUN(testWriteBlockRejectsOutOfRange);
    RUN(testWriteBlockRejectsBeyondLastPieceSize);
    RUN(testAssembleFileConcatenatesInOrder);
    RUN(testAssembleFileSkipsIncompletePieces);
    return TEST_REPORT();
}
