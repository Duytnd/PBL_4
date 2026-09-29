#include "core/hash_utils.h"

#include <openssl/sha.h>

#include <string>

namespace p2p {

std::string sha1Digest(const std::string& data) {
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return std::string(reinterpret_cast<const char*>(digest), SHA_DIGEST_LENGTH);
}

std::string sha1Hex(const std::string& data) {
    const std::string digest = sha1Digest(data);

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(hex[(byte >> 4) & 0xF]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

std::string computeInfoHash(const std::string& encodedInfo) {
    return sha1Hex(encodedInfo);
}

std::string computeInfoHash(const BencodeDict& infoDict) {
    return computeInfoHash(encodeBencode(infoDict));
}

}  // namespace p2p
