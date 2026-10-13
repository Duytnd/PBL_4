#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pbl4::network {

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using InfoHash = std::array<Byte, 20>;
using PeerId = std::array<Byte, 20>;

constexpr std::size_t kHandshakeSize = 68;
constexpr std::size_t kInfoHashSize = 20;
constexpr std::size_t kPeerIdSize = 20;
constexpr std::size_t kReservedSize = 8;

// BitTorrent protocol string
constexpr char kProtocolString[] = "BitTorrent protocol";
constexpr std::size_t kProtocolStringLength = 19;

// Default defensive limits (BEP 3)
constexpr std::uint32_t kMaxPeerMessageLength = 128 * 1024;  // 128 KiB
constexpr std::uint32_t kStandardBlockSize = 16 * 1024;      // 16 KiB
constexpr std::uint32_t kMaxBlockSize = 128 * 1024;          // 128 KiB max block

enum class PeerMessageId : Byte {
	Choke = 0,
	Unchoke = 1,
	Interested = 2,
	NotInterested = 3,
	Have = 4,
	Bitfield = 5,
	Request = 6,
	Piece = 7,
	Cancel = 8,
	Port = 9,
	Extended = 20
};

struct PeerHandshake {
	std::array<Byte, 8> reserved{};
	InfoHash info_hash{};
	PeerId peer_id{};

	// BEP 10 Extension protocol: bit 0x10 at byte index 5 (20th bit from right in reserved)
	bool supports_extension() const noexcept {
		return (reserved[5] & 0x10) != 0;
	}

	void set_supports_extension(bool enable = true) noexcept {
		if (enable) {
			reserved[5] |= 0x10;
		} else {
			reserved[5] &= static_cast<Byte>(~0x10);
		}
	}

	// BEP 5 DHT: bit 0x01 at byte index 7
	bool supports_dht() const noexcept {
		return (reserved[7] & 0x01) != 0;
	}

	void set_supports_dht(bool enable = true) noexcept {
		if (enable) {
			reserved[7] |= 0x01;
		} else {
			reserved[7] &= static_cast<Byte>(~0x01);
		}
	}
};

struct PeerMessage {
	bool keep_alive = false;
	Byte id = 0;
	Bytes payload;
};

// Strongly typed payload structures for BEP 3 messages
struct RequestPayload {
	std::uint32_t index = 0;
	std::uint32_t begin = 0;
	std::uint32_t length = 0;

	bool operator==(const RequestPayload& other) const noexcept {
		return index == other.index && begin == other.begin && length == other.length;
	}
};

struct PiecePayload {
	std::uint32_t index = 0;
	std::uint32_t begin = 0;
	Bytes data;

	bool operator==(const PiecePayload& other) const noexcept {
		return index == other.index && begin == other.begin && data == other.data;
	}
};

struct CancelPayload {
	std::uint32_t index = 0;
	std::uint32_t begin = 0;
	std::uint32_t length = 0;

	bool operator==(const CancelPayload& other) const noexcept {
		return index == other.index && begin == other.begin && length == other.length;
	}
};

struct HavePayload {
	std::uint32_t index = 0;

	bool operator==(const HavePayload& other) const noexcept {
		return index == other.index;
	}
};

class PeerProtocolError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

namespace peer_protocol {

// Handshake encode & decode
Bytes encode_handshake(const PeerHandshake& handshake);
PeerHandshake decode_handshake(const Bytes& bytes);
PeerHandshake decode_handshake(const Byte* data, std::size_t size);

// Raw message frame encode & decode
Bytes encode_message(const PeerMessage& message,
					 std::uint32_t max_length = kMaxPeerMessageLength);
PeerMessage decode_message(const Bytes& frame,
						   std::uint32_t max_length = kMaxPeerMessageLength);
PeerMessage decode_message(const Byte* data, std::size_t size,
						   std::uint32_t max_length = kMaxPeerMessageLength);

// Message constructors (Helpers for BEP 3)
PeerMessage make_keep_alive();
PeerMessage make_choke();
PeerMessage make_unchoke();
PeerMessage make_interested();
PeerMessage make_not_interested();
PeerMessage make_have(std::uint32_t piece_index);
PeerMessage make_bitfield(const Bytes& bitfield_bytes);
PeerMessage make_bitfield(const std::vector<bool>& bits);
PeerMessage make_request(std::uint32_t index, std::uint32_t begin,
						 std::uint32_t length = kStandardBlockSize);
PeerMessage make_piece(std::uint32_t index, std::uint32_t begin, const Bytes& data);
PeerMessage make_piece(std::uint32_t index, std::uint32_t begin, const Byte* data,
					   std::size_t size);
PeerMessage make_cancel(std::uint32_t index, std::uint32_t begin, std::uint32_t length);
PeerMessage make_port(std::uint16_t port);
PeerMessage make_extended(Byte ext_id, const Bytes& payload);

// Payload parsers
HavePayload parse_have(const PeerMessage& message);
RequestPayload parse_request(const PeerMessage& message);
PiecePayload parse_piece(const PeerMessage& message);
CancelPayload parse_cancel(const PeerMessage& message);
std::uint16_t parse_port(const PeerMessage& message);
std::vector<bool> parse_bitfield(const PeerMessage& message, std::size_t num_pieces = 0);

// Utility
const char* message_id_to_string(Byte id, bool keep_alive = false) noexcept;
void validate_message_payload(Byte id, std::size_t payload_size,
							  std::uint32_t max_length = kMaxPeerMessageLength);

}  // namespace peer_protocol
}  // namespace pbl4::network

namespace p2p::network {
	using namespace pbl4::network;
}
