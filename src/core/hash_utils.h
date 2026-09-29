#pragma once

#include "core/bencode.h"

#include <string>

namespace p2p {

std::string sha1Digest(const std::string& data);
std::string sha1Hex(const std::string& data);
std::string computeInfoHash(const std::string& encodedInfo);
std::string computeInfoHash(const BencodeDict& infoDict);

}  // namespace p2p
