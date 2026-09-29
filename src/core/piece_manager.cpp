#include "core/piece_manager.h"

#include "core/hash_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace p2p {

namespace {

std::size_t clampPieceSize(std::size_t totalLength, std::size_t pieceLength, std::size_t pieceIndex) {
    const std::size_t start = pieceIndex * pieceLength;
    if (start >= totalLength) {
        return 0;
    }
    return std::min(pieceLength, totalLength - start);
}

}  // namespace

PieceManager::PieceManager(
    std::size_t pieceCount,
    std::size_t pieceLength,
    std::size_t totalLength,
    std::vector<std::string> pieceHashes)
    : pieceCount_(pieceCount),
      pieceLength_(pieceLength == 0 ? 1 : pieceLength),
      totalLength_(totalLength),
      pieceHashes_(std::move(pieceHashes)) {
    pieceHashes_.resize(pieceCount_, "");
    pieces_.resize(pieceCount_, "");
}

std::size_t PieceManager::pieceCount() const {
    return pieceCount_;
}

std::size_t PieceManager::pieceLength() const {
    return pieceLength_;
}

std::size_t PieceManager::totalLength() const {
    return totalLength_;
}

std::size_t PieceManager::pieceOffset(std::size_t pieceIndex) const {
    return pieceIndex * pieceLength_;
}

std::size_t PieceManager::pieceSize(std::size_t pieceIndex) const {
    if (pieceIndex >= pieceCount_) {
        return 0;
    }
    return clampPieceSize(totalLength_, pieceLength_, pieceIndex);
}

bool PieceManager::hasPiece(std::size_t pieceIndex) const {
    return isPieceComplete(pieceIndex);
}

bool PieceManager::isPieceComplete(std::size_t pieceIndex) const {
    if (pieceIndex >= pieceCount_) {
        return false;
    }
    return pieces_[pieceIndex].size() == pieceSize(pieceIndex);
}

bool PieceManager::verifyPiece(std::size_t pieceIndex) const {
    if (pieceIndex >= pieceCount_) {
        return false;
    }
    if (pieceHashes_.size() <= pieceIndex || pieceHashes_[pieceIndex].empty()) {
        return false;
    }
    if (!isPieceComplete(pieceIndex)) {
        return false;
    }
    const std::string expected = pieceHashes_[pieceIndex];
    const std::string actual = sha1Digest(pieces_[pieceIndex]);
    return actual == expected;
}

bool PieceManager::writeBlock(std::size_t pieceIndex, std::size_t offset, const std::string& blockData) {
    return writeBlock(pieceIndex, offset, blockData.size(), blockData);
}

bool PieceManager::writeBlock(
    std::size_t pieceIndex,
    std::size_t offset,
    std::size_t length,
    const std::string& blockData) {
    if (pieceIndex >= pieceCount_) {
        return false;
    }

    if (offset > pieceLength_ || length > pieceLength_ || offset + length > pieceLength_) {
        return false;
    }

    const std::size_t maxPieceSize = pieceSize(pieceIndex);
    if (offset + length > maxPieceSize) {
        return false;
    }

    if (blockData.size() < length) {
        return false;
    }

    std::string& data = pieces_[pieceIndex];
    if (offset + length > data.size()) {
        data.resize(offset + length, '\0');
    }

    for (std::size_t i = 0; i < length; ++i) {
        data[offset + i] = blockData[i];
    }
    return true;
}

std::string PieceManager::assembleFile() const {
    std::string output;
    output.reserve(totalLength_);

    for (std::size_t pieceIndex = 0; pieceIndex < pieceCount_; ++pieceIndex) {
        const std::size_t size = pieceSize(pieceIndex);
        if (size == 0) {
            continue;
        }
        const std::string& pieceBytes = pieces_[pieceIndex];
        if (pieceBytes.size() < size) {
            continue;
        }
        output.append(pieceBytes.data(), size);
    }

    if (output.size() > totalLength_) {
        output.resize(totalLength_);
    }

    return output;
}

void PieceManager::writeToFile(const std::filesystem::path& filePath) const {
    const std::string payload = assembleFile();
    const auto parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("PieceManager: failed to open output file");
    }
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

}  // namespace p2p
