#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p::network::udp_rendezvous {

enum class Opcode : std::uint8_t {
    Register = 1,
    Punch = 2,
    Result = 0x81,
    Notify = 0x82
};

enum class Status : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    PeerNotFound = 2,
    EndpointNotReady = 3
};

// Khoá phiên 8 byte gắn với peer_id (client tự sinh ngẫu nhiên, gửi kèm HTTP announce qua tham số `key` dạng hex).
// peer_id lộ ra cho mọi peer đã bắt tay với mình, nên chỉ biết peer_id là chưa đủ để đăng ký/đổi endpoint UDP của
// peer đó: tracker còn đòi đúng khoá. 8 byte 0 nghĩa là "không có khoá" (client cũ), tracker không áp dụng kiểm tra.
constexpr std::size_t kKeySize = 8;

struct Request {
    Opcode opcode = Opcode::Register;
    std::string info_hash;
    std::string peer_id;
    std::uint16_t peer_tcp_port = 0;
    std::string key = std::string(kKeySize, '\0');
    std::string target_ip;
    std::uint16_t target_tcp_port = 0;
    std::uint32_t transaction_id = 0;
};

struct Result {
    Status status = Status::Invalid;
    std::string info_hash;
    std::uint32_t transaction_id = 0;
    std::string ip;
    std::uint16_t port = 0;
};

struct Notification {
    std::string info_hash;
    std::string tcp_ip;
    std::uint16_t tcp_port = 0;
    std::string ip;
    std::uint16_t port = 0;
};

std::string encode_register_request(
    const std::string& info_hash,
    const std::string& peer_id,
    std::uint16_t peer_tcp_port,
    const std::string& key = std::string(kKeySize, '\0'));
std::string encode_punch_request(
    const std::string& info_hash,
    const std::string& peer_id,
    std::uint16_t peer_tcp_port,
    const std::string& target_ip,
    std::uint16_t target_tcp_port,
    std::uint32_t transaction_id,
    const std::string& key = std::string(kKeySize, '\0'));
bool decode_request(const char* data, std::size_t size, Request& request) noexcept;

std::string encode_register_result(Status status);
std::string encode_punch_result(
    Status status,
    const std::string& info_hash,
    std::uint32_t transaction_id,
    const std::string& ip = {},
    std::uint16_t port = 0);
std::string encode_notification(
    const std::string& info_hash,
    const std::string& tcp_ip,
    std::uint16_t tcp_port,
    const std::string& ip,
    std::uint16_t port);
bool decode_punch_result(const char* data, std::size_t size, Result& result) noexcept;
bool decode_notification(const char* data, std::size_t size, Notification& notification) noexcept;

}  // namespace p2p::network::udp_rendezvous