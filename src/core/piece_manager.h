#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace p2p {

// Quản lý dữ liệu của từng piece trong torrent.
// Mỗi piece có thể được ghi từng block, kiểm tra đầy đủ, verify bằng SHA-1,
// rồi ghép lại thành toàn bộ file khi download xong.
class PieceManager {
public:
    PieceManager(
        std::size_t pieceCount,
        std::size_t pieceLength,
        std::size_t totalLength,
        std::vector<std::string> pieceHashes = {});

    [[nodiscard]] std::size_t pieceCount() const;
    [[nodiscard]] std::size_t pieceLength() const;
    [[nodiscard]] std::size_t totalLength() const;
    [[nodiscard]] std::size_t pieceOffset(std::size_t pieceIndex) const;
    [[nodiscard]] std::size_t pieceSize(std::size_t pieceIndex) const;

    [[nodiscard]] bool hasPiece(std::size_t pieceIndex) const;
    [[nodiscard]] bool isPieceComplete(std::size_t pieceIndex) const;
    [[nodiscard]] bool verifyPiece(std::size_t pieceIndex) const;

    bool writeBlock(std::size_t pieceIndex, std::size_t offset, const std::string& blockData);
    bool writeBlock(std::size_t pieceIndex, std::size_t offset, std::size_t length, const std::string& blockData);

    [[nodiscard]] std::string assembleFile() const;
    void writeToFile(const std::filesystem::path& filePath) const;

private:
    std::size_t pieceCount_;
    std::size_t pieceLength_;
    std::size_t totalLength_;
    std::vector<std::string> pieceHashes_;
    std::vector<std::string> pieces_;
};

}  // namespace p2p
