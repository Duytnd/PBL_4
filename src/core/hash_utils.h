#pragma once

#include "core/bencode.h"

#include <openssl/sha.h>
#include <string>

namespace p2p {

// Tạo digest SHA-1 và info hash cho torrent metadata.
// Dùng trong việc xác minh từng piece và tính hash của dictionary info.
std::string sha1Digest(const std::string& data);
std::string sha1Hex(const std::string& data);
std::string computeInfoHash(const std::string& encodedInfo);
std::string computeInfoHash(const BencodeDict& infoDict);

// SHA-1 context dùng để hash dữ liệu theo từng chunk (streaming),
// tránh load toàn bộ file vào RAM.
class Sha1Context {
public:
    Sha1Context();
    void update(const char* data, std::size_t size);
    // Trả về 20-byte raw digest
    std::string finalize();
private:
    SHA_CTX ctx_;
};

}  // namespace p2p
