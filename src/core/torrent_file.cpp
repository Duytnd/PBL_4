#include "core/torrent_file.h"

#include <fstream>
#include <stdexcept>

namespace p2p {
namespace {

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

}  // namespace

TorrentFileMetadata TorrentFile::parse(const std::string& encodedData) {
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

    metadata.infoRaw = encodeBencode(infoIt->second);

    const auto nameIt = infoDict->find("name");
    if (nameIt == infoDict->end()) {
        throw std::invalid_argument("Torrent metadata: missing info.name");
    }
    const auto* nameValue = asString(nameIt->second);
    if (nameValue == nullptr) {
        throw std::invalid_argument("Torrent metadata: info.name must be a string");
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
        metadata.length = static_cast<std::uint64_t>(*lengthValue);
    }

    const auto filesIt = infoDict->find("files");
    if (filesIt != infoDict->end()) {
        const auto* filesList = filesIt->second.get_if<BencodeList>();
        if (filesList == nullptr) {
            throw std::invalid_argument("Torrent metadata: info.files must be a list");
        }
        for (const auto& fileEntry : *filesList) {
            const auto* fileDict = asDict(fileEntry);
            if (fileDict == nullptr) {
                throw std::invalid_argument("Torrent metadata: each file entry must be a dictionary");
            }
            const auto pathIt = fileDict->find("path");
            if (pathIt == fileDict->end()) {
                continue;
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
            if (!pathValue.empty()) {
                metadata.filePaths.push_back(pathValue);
            }
        }
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
