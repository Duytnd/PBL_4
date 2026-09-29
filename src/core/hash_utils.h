#pragma once

#include "core/bencode.h"

#include <string>

namespace p2p {

// Tạo digest SHA-1 và info hash cho torrent metadata.
// Dùng trong việc xác minh từng piece và tính hash của dictionary info.
std::string sha1Digest(const std::string& data);
std::string sha1Hex(const std::string& data);
std::string computeInfoHash(const std::string& encodedInfo);
std::string computeInfoHash(const BencodeDict& infoDict);

}  // namespace p2p
