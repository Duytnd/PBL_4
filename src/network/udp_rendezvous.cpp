#include "network/udp_rendezvous.h"

#include <array>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace p2p::network::udp_rendezvous {
namespace {

constexpr std::array<char, 4> kMagic{'P', '4', 'U', 'T'};
constexpr char kVersion = 1;
constexpr std::size_t kRequestHeaderSize = 48;
constexpr std::size_t kPunchRequestSize = 58;
constexpr std::size_t kPunchResultSize = 37;
constexpr std::size_t kNotificationSize = 39;

bool has_header(const char* data, std::size_t size, Opcode opcode) noexcept {
    return data != nullptr && size >= 6 &&
           std::string_view(data, kMagic.size()) == std::string_view(kMagic.data(), kMagic.size()) &&
           data[4] == kVersion && static_cast<std::uint8_t>(data[5]) == static_cast<std::uint8_t>(opcode);
}

bool parse_ipv4(const std::string& ip, std::array<std::uint8_t, 4>& octets) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const std::size_t end = index == octets.size() - 1 ? ip.size() : ip.find('.', start);
        if (end == std::string::npos || end == start) {
            return false;
        }
        unsigned value = 0;
        const char* first = ip.data() + start;
        const char* last = ip.data() + end;
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last || value > 255) {
            return false;
        }
        octets[index] = static_cast<std::uint8_t>(value);
        start = end + 1;
    }
    return start == ip.size() + 1;
}

std::string format_ipv4(const char* data) {
    return std::to_string(static_cast<unsigned char>(data[0])) + "." +
           std::to_string(static_cast<unsigned char>(data[1])) + "." +
           std::to_string(static_cast<unsigned char>(data[2])) + "." +
           std::to_string(static_cast<unsigned char>(data[3]));
}

void append_header(std::string& message, Opcode opcode) {
    message.append(kMagic.data(), kMagic.size());
    message.push_back(kVersion);
    message.push_back(static_cast<char>(opcode));
}

void append_port(std::string& message, std::uint16_t port) {
    message.push_back(static_cast<char>((port >> 8) & 0xff));
    message.push_back(static_cast<char>(port & 0xff));
}

std::uint16_t read_port(const char* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
        static_cast<std::uint16_t>(static_cast<unsigned char>(data[1])));
}

void append_transaction(std::string& message, std::uint32_t transaction_id) {
    message.push_back(static_cast<char>((transaction_id >> 24) & 0xff));
    message.push_back(static_cast<char>((transaction_id >> 16) & 0xff));
    message.push_back(static_cast<char>((transaction_id >> 8) & 0xff));
    message.push_back(static_cast<char>(transaction_id & 0xff));
}

std::uint32_t read_transaction(const char* data) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

bool valid_info_hash_and_peer_id(const std::string& info_hash, const std::string& peer_id) {
    return info_hash.size() == 20 && peer_id.size() == 20;
}

}  // namespace

std::string encode_register_request(
    const std::string& info_hash,
    const std::string& peer_id,
    std::uint16_t peer_tcp_port) {
    if (!valid_info_hash_and_peer_id(info_hash, peer_id) || peer_tcp_port == 0) {
        throw std::invalid_argument("rendezvous registration has invalid fields");
    }
    std::string message;
    message.reserve(kRequestHeaderSize);
    append_header(message, Opcode::Register);
    message.append(info_hash);
    message.append(peer_id);
    append_port(message, peer_tcp_port);
    return message;
}

std::string encode_punch_request(
    const std::string& info_hash,
    const std::string& peer_id,
    std::uint16_t peer_tcp_port,
    const std::string& target_ip,
    std::uint16_t target_tcp_port,
    std::uint32_t transaction_id) {
    std::array<std::uint8_t, 4> octets{};
    if (!valid_info_hash_and_peer_id(info_hash, peer_id) || peer_tcp_port == 0 || target_tcp_port == 0 ||
        !parse_ipv4(target_ip, octets)) {
        throw std::invalid_argument("rendezvous punch request has invalid fields");
    }
    std::string message;
    message.reserve(kPunchRequestSize);
    append_header(message, Opcode::Punch);
    message.append(info_hash);
    message.append(peer_id);
    append_port(message, peer_tcp_port);
    for (const auto octet : octets) {
        message.push_back(static_cast<char>(octet));
    }
    append_port(message, target_tcp_port);
    append_transaction(message, transaction_id);
    return message;
}

bool decode_request(const char* data, std::size_t size, Request& request) noexcept {
    if (data == nullptr || size < kRequestHeaderSize || data[4] != kVersion) {
        return false;
    }
    const auto opcode = static_cast<Opcode>(static_cast<std::uint8_t>(data[5]));
    if (opcode == Opcode::Register) {
        if (size != kRequestHeaderSize) {
            return false;
        }
    } else if (opcode == Opcode::Punch) {
        if (size != kPunchRequestSize) {
            return false;
        }
    } else {
        return false;
    }
    if (!has_header(data, size, opcode)) {
        return false;
    }

    request.opcode = opcode;
    request.info_hash.assign(data + 6, 20);
    request.peer_id.assign(data + 26, 20);
    request.peer_tcp_port = read_port(data + 46);
    if (request.peer_tcp_port == 0) {
        return false;
    }
    if (opcode == Opcode::Punch) {
        request.target_ip = format_ipv4(data + 48);
        request.target_tcp_port = read_port(data + 52);
        request.transaction_id = read_transaction(data + 54);
        if (request.target_tcp_port == 0 || request.target_ip == "0.0.0.0") {
            return false;
        }
    }
    return true;
}

std::string encode_register_result(Status status) {
    std::string message;
    message.reserve(7);
    append_header(message, Opcode::Result);
    message.push_back(static_cast<char>(status));
    return message;
}

std::string encode_punch_result(
    Status status,
    const std::string& info_hash,
    std::uint32_t transaction_id,
    const std::string& ip,
    std::uint16_t port) {
    if (info_hash.size() != 20) {
        throw std::invalid_argument("rendezvous result info_hash must be 20 bytes");
    }
    std::array<std::uint8_t, 4> octets{};
    if (status == Status::Ok && (port == 0 || !parse_ipv4(ip, octets))) {
        throw std::invalid_argument("successful rendezvous result requires a valid IPv4 endpoint");
    }
    std::string message;
    message.reserve(kPunchResultSize);
    append_header(message, Opcode::Result);
    message.push_back(static_cast<char>(status));
    message.append(info_hash);
    append_transaction(message, transaction_id);
    for (const auto octet : octets) {
        message.push_back(static_cast<char>(octet));
    }
    append_port(message, status == Status::Ok ? port : 0);
    return message;
}

std::string encode_notification(
    const std::string& info_hash,
    const std::string& tcp_ip,
    std::uint16_t tcp_port,
    const std::string& ip,
    std::uint16_t port) {
    std::array<std::uint8_t, 4> tcpOctets{};
    std::array<std::uint8_t, 4> octets{};
    if (info_hash.size() != 20 || tcp_port == 0 || port == 0 ||
        !parse_ipv4(tcp_ip, tcpOctets) || !parse_ipv4(ip, octets)) {
        throw std::invalid_argument("rendezvous notification has invalid fields");
    }
    std::string message;
    message.reserve(kNotificationSize);
    append_header(message, Opcode::Notify);
    message.push_back(static_cast<char>(Status::Ok));
    message.append(info_hash);
    for (const auto octet : tcpOctets) {
        message.push_back(static_cast<char>(octet));
    }
    append_port(message, tcp_port);
    for (const auto octet : octets) {
        message.push_back(static_cast<char>(octet));
    }
    append_port(message, port);
    return message;
}

bool decode_punch_result(const char* data, std::size_t size, Result& result) noexcept {
    if (!has_header(data, size, Opcode::Result) || size != kPunchResultSize) {
        return false;
    }
    result.status = static_cast<Status>(static_cast<std::uint8_t>(data[6]));
    result.info_hash.assign(data + 7, 20);
    result.transaction_id = read_transaction(data + 27);
    result.ip = format_ipv4(data + 31);
    result.port = read_port(data + 35);
    if (result.status == Status::Ok) {
        return result.port != 0 && result.ip != "0.0.0.0";
    }
    return result.port == 0;
}

bool decode_notification(const char* data, std::size_t size, Notification& notification) noexcept {
    if (!has_header(data, size, Opcode::Notify) || size != kNotificationSize ||
        static_cast<std::uint8_t>(data[6]) != static_cast<std::uint8_t>(Status::Ok)) {
        return false;
    }
    notification.info_hash.assign(data + 7, 20);
    notification.tcp_ip = format_ipv4(data + 27);
    notification.tcp_port = read_port(data + 31);
    notification.ip = format_ipv4(data + 33);
    notification.port = read_port(data + 37);
    return notification.tcp_ip != "0.0.0.0" && notification.tcp_port != 0 &&
           notification.ip != "0.0.0.0" && notification.port != 0;
}

}  // namespace p2p::network::udp_rendezvous