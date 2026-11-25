#include "core/torrent_file.h"

#include "core/path_sanitizer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

namespace p2p {
namespace {

// Các helper nhỏ để đọc dữ liệu Bencode theo kiểu mong muốn.
// Ví dụ: nếu value là map, ta lấy ra dict; nếu là string, ta lấy ra string.
const BencodeDict* asDict(const BencodeValue& value) {
    return value.get_if<BencodeDict>();
}

const std::string* asString(const BencodeValue& value) {
    return value.get_if<std::string>();
}

const std::int64_t* asInt64(const BencodeValue& value) {
    return value.get_if<std::int64_t>();
}

std::vector<std::string> parsePieces(const std::string& rawPieces) {
    if (rawPieces.empty()) {
        return {};
    }
    if (rawPieces.size() % 20 != 0) {
        throw std::invalid_argument("Torrent metadata: pieces length must be divisible by 20");
    }

    std::vector<std::string> pieces;
    pieces.reserve(rawPieces.size() / 20);
    for (std::size_t i = 0; i < rawPieces.size(); i += 20) {
        pieces.emplace_back(rawPieces.substr(i, 20));
    }
    return pieces;
}

// Giới hạn piece length: 64 MiB (torrent thật thường 16 KiB - 16 MiB).
constexpr std::int64_t kMaxPieceLength = std::int64_t(1) << 26;

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// Từ chối các đường dẫn trùng nhau không phân biệt hoa/thường, hoặc một file nằm đúng chỗ của
// thư mục chứa file khác (file "a" cạnh "a/b"): ghi ra đĩa sẽ đè lên nhau hoặc thất bại.
void rejectCollidingPaths(const std::vector<std::string>& paths) {
    std::set<std::string> seen;
    for (const auto& path : paths) {
        if (!seen.insert(toLowerAscii(path)).second) {
            throw std::invalid_argument("Torrent metadata: duplicate file path");
        }
    }
    for (const auto& lowered : seen) {
        for (std::size_t slash = lowered.find('/'); slash != std::string::npos;
             slash = lowered.find('/', slash + 1)) {
            if (seen.count(lowered.substr(0, slash)) > 0) {
                throw std::invalid_argument("Torrent metadata: file path conflicts with a directory");
            }
        }
    }
}

}  // namespace

TorrentFileMetadata TorrentFile::parse(const std::string& encodedData) {
    // Parse file .torrent từ Bencode -> struct TorrentFileMetadata.
    // Đây là bước chuyển từ dữ liệu raw (dạng text bencode) sang cấu trúc dữ liệu dễ xử lý.
    const auto root = decodeBencode(encodedData);
    const auto* rootDict = asDict(root);
    if (rootDict == nullptr) {
        throw std::invalid_argument("Torrent metadata: top-level value must be a dictionary");
    }

    TorrentFileMetadata metadata;

    const auto announceIt = rootDict->find("announce");
    if (announceIt != rootDict->end()) {
        const auto* announceValue = asString(announceIt->second);
        if (announceValue == nullptr) {
            throw std::invalid_argument("Torrent metadata: announce must be a string");
        }
        metadata.announce = *announceValue;
    }

    const auto infoIt = rootDict->find("info");
    if (infoIt == rootDict->end()) {
        throw std::invalid_argument("Torrent metadata: missing info dictionary");
    }

    const auto* infoDict = asDict(infoIt->second);
    if (infoDict == nullptr) {
        throw std::invalid_argument("Torrent metadata: info must be a dictionary");
    }

    // info-hash phải tính từ ĐÚNG byte gốc của giá trị "info" (không mã hóa lại,
    // vì mã hóa lại sắp xếp khóa và gộp khóa trùng nên cho hash khác bầy đàn).
    {
        std::size_t infoBegin = 0;
        std::size_t infoEnd = 0;
        if (!findDictValueSpan(encodedData, "info", infoBegin, infoEnd)) {
            throw std::invalid_argument("Torrent metadata: missing info dictionary");
        }
        metadata.infoRaw = encodedData.substr(infoBegin, infoEnd - infoBegin);
    }

    const auto nameIt = infoDict->find("name");
    if (nameIt == infoDict->end()) {
        throw std::invalid_argument("Torrent metadata: missing info.name");
    }
    const auto* nameValue = asString(nameIt->second);
    if (nameValue == nullptr) {
        throw std::invalid_argument("Torrent metadata: info.name must be a string");
    }
    // name được ghép vào download_dir: chặn traversal / đường dẫn tuyệt đối từ torrent độc.
    if (!PathSanitizer::isSafeRelativePath(*nameValue)) {
        throw std::invalid_argument("Torrent metadata: info.name is not a safe relative path");
    }
    metadata.name = *nameValue;

    const auto pieceLengthIt = infoDict->find("piece length");
    if (pieceLengthIt == infoDict->end()) {
        throw std::invalid_argument("Torrent metadata: missing info.piece length");
    }
    const auto* pieceLengthValue = asInt64(pieceLengthIt->second);
    if (pieceLengthValue == nullptr) {
        throw std::invalid_argument("Torrent metadata: info.piece length must be an integer");
    }
    if (*pieceLengthValue <= 0) {
        throw std::invalid_argument("Torrent metadata: info.piece length must be positive");
    }
    if (*pieceLengthValue > kMaxPieceLength) {
        throw std::invalid_argument("Torrent metadata: info.piece length is too large");
    }
    metadata.pieceLength = *pieceLengthValue;

    const auto piecesIt = infoDict->find("pieces");
    if (piecesIt == infoDict->end()) {
        throw std::invalid_argument("Torrent metadata: missing info.pieces");
    }
    const auto* piecesValue = asString(piecesIt->second);
    if (piecesValue == nullptr) {
        throw std::invalid_argument("Torrent metadata: info.pieces must be a string");
    }
    metadata.pieces = parsePieces(*piecesValue);

    const auto lengthIt = infoDict->find("length");
    if (lengthIt != infoDict->end()) {
        const auto* lengthValue = asInt64(lengthIt->second);
        if (lengthValue == nullptr) {
            throw std::invalid_argument("Torrent metadata: info.length must be an integer");
        }
        if (*lengthValue < 0) {
            throw std::invalid_argument("Torrent metadata: info.length must not be negative");
        }
        metadata.length = static_cast<std::uint64_t>(*lengthValue);
    }

    const auto filesIt = infoDict->find("files");
    if (filesIt != infoDict->end()) {
        const auto* filesList = filesIt->second.get_if<BencodeList>();
        if (filesList == nullptr) {
            throw std::invalid_argument("Torrent metadata: info.files must be a list");
        }
        std::uint64_t totalLength = 0;
        for (const auto& fileEntry : *filesList) {
            const auto* fileDict = asDict(fileEntry);
            if (fileDict == nullptr) {
                throw std::invalid_argument("Torrent metadata: each file entry must be a dictionary");
            }
            const auto pathIt = fileDict->find("path");
            if (pathIt == fileDict->end()) {
                throw std::invalid_argument("Torrent metadata: file entry is missing path");
            }
            const auto* pathList = pathIt->second.get_if<BencodeList>();
            if (pathList == nullptr) {
                throw std::invalid_argument("Torrent metadata: file path must be a list");
            }
            std::string pathValue;
            for (const auto& part : *pathList) {
                const auto* partString = asString(part);
                if (partString == nullptr) {
                    throw std::invalid_argument("Torrent metadata: path members must be strings");
                }
                if (!pathValue.empty()) {
                    pathValue += "/";
                }
                pathValue += *partString;
            }
            if (pathValue.empty() || !PathSanitizer::isSafeRelativePath(pathValue)) {
                throw std::invalid_argument("Torrent metadata: unsafe file path");
            }
            const auto fileLengthIt = fileDict->find("length");
            if (fileLengthIt == fileDict->end()) {
                throw std::invalid_argument("Torrent metadata: file entry is missing length");
            }
            const auto* fileLength = asInt64(fileLengthIt->second);
            if (fileLength == nullptr || *fileLength < 0) {
                throw std::invalid_argument("Torrent metadata: file length must be a non-negative integer");
            }
            const auto len = static_cast<std::uint64_t>(*fileLength);
            if (len > static_cast<std::uint64_t>(INT64_MAX) ||
                totalLength > static_cast<std::uint64_t>(INT64_MAX) - len) {
                throw std::invalid_argument("Torrent metadata: total length overflow");
            }
            totalLength += len;
            metadata.filePaths.push_back(pathValue);
            metadata.files.push_back(FileEntry{pathValue, len});
        }
        rejectCollidingPaths(metadata.filePaths);
        if (!metadata.files.empty()) {
            metadata.length = totalLength;
        }
    }

    // Kiểm tra nhất quán: tổng dung lượng > 0 và số hash khớp ceil(length / pieceLength).
    // Nếu không, tải sẽ bị kẹt (piece size 0) hoặc đọc ra ngoài phạm vi.
    if (metadata.length == 0) {
        throw std::invalid_argument("Torrent metadata: total length must be positive");
    }
    const auto pieceLength = static_cast<std::uint64_t>(metadata.pieceLength);
    const std::uint64_t expectedPieces = metadata.length / pieceLength + (metadata.length % pieceLength != 0 ? 1 : 0);
    if (metadata.pieces.size() != expectedPieces) {
        throw std::invalid_argument("Torrent metadata: piece count does not match length and piece length");
    }

    return metadata;
}

TorrentFileMetadata TorrentFile::parseFile(const std::string& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open torrent file: " + filePath);
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parse(content);
}

}  // namespace p2p
