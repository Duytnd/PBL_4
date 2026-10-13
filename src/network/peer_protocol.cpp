#include "peer_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pbl4::network {
namespace peer_protocol {
namespace {

void append_u16(Bytes& output, std::uint16_t value) {
	output.push_back(static_cast<Byte>((value >> 8) & 0xff));
	output.push_back(static_cast<Byte>(value & 0xff));
}

void append_u32(Bytes& output, std::uint32_t value) {
	output.push_back(static_cast<Byte>((value >> 24) & 0xff));
	output.push_back(static_cast<Byte>((value >> 16) & 0xff));
	output.push_back(static_cast<Byte>((value >> 8) & 0xff));
	output.push_back(static_cast<Byte>(value & 0xff));
}

std::uint16_t read_u16(const Byte* data, std::size_t offset) {
	return (static_cast<std::uint16_t>(data[offset]) << 8) |
		   static_cast<std::uint16_t>(data[offset + 1]);
}

std::uint32_t read_u32(const Byte* data, std::size_t offset) {
	return (static_cast<std::uint32_t>(data[offset]) << 24) |
		   (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
		   (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
		   static_cast<std::uint32_t>(data[offset + 3]);
}

}  // namespace

const char* message_id_to_string(Byte id, bool keep_alive) noexcept {
	if (keep_alive) {
		return "keep-alive";
	}
	switch (static_cast<PeerMessageId>(id)) {
		case PeerMessageId::Choke:
			return "choke";
		case PeerMessageId::Unchoke:
			return "unchoke";
		case PeerMessageId::Interested:
			return "interested";
		case PeerMessageId::NotInterested:
			return "not interested";
		case PeerMessageId::Have:
			return "have";
		case PeerMessageId::Bitfield:
			return "bitfield";
		case PeerMessageId::Request:
			return "request";
		case PeerMessageId::Piece:
			return "piece";
		case PeerMessageId::Cancel:
			return "cancel";
		case PeerMessageId::Port:
			return "port";
		case PeerMessageId::Extended:
			return "extended";
		default:
			return "unknown";
	}
}

void validate_message_payload(Byte id, std::size_t payload_size, std::uint32_t max_length) {
	switch (static_cast<PeerMessageId>(id)) {
		case PeerMessageId::Choke:
		case PeerMessageId::Unchoke:
		case PeerMessageId::Interested:
		case PeerMessageId::NotInterested:
			if (payload_size != 0) {
				throw PeerProtocolError("control message must not have a payload");
			}
			break;
		case PeerMessageId::Have:
			if (payload_size != 4) {
				throw PeerProtocolError("have message payload must be 4 bytes");
			}
			break;
		case PeerMessageId::Bitfield:
			if (payload_size == 0) {
				throw PeerProtocolError("bitfield message must not be empty");
			}
			break;
		case PeerMessageId::Request:
		case PeerMessageId::Cancel:
			if (payload_size != 12) {
				throw PeerProtocolError("request/cancel message payload must be 12 bytes");
			}
			break;
		case PeerMessageId::Piece:
			if (payload_size < 9) {
				throw PeerProtocolError("piece message must contain an 8-byte header and data");
			}
			if (payload_size - 8 > max_length) {
				throw PeerProtocolError("piece message block data exceeds maximum size");
			}
			break;
		case PeerMessageId::Port:
			if (payload_size != 2) {
				throw PeerProtocolError("port message payload must be 2 bytes");
			}
			break;
		case PeerMessageId::Extended:
			if (payload_size < 1) {
				throw PeerProtocolError("extended message must contain an extension ID");
			}
			break;
		default:
			// Allow unknown message IDs for forwards compatibility (BEP 3 recommendation)
			break;
	}
}

Bytes encode_handshake(const PeerHandshake& handshake) {
	Bytes bytes;
	bytes.reserve(kHandshakeSize);
	bytes.push_back(static_cast<Byte>(kProtocolStringLength));
	bytes.insert(bytes.end(), kProtocolString, kProtocolString + kProtocolStringLength);
	bytes.insert(bytes.end(), handshake.reserved.begin(), handshake.reserved.end());
	bytes.insert(bytes.end(), handshake.info_hash.begin(), handshake.info_hash.end());
	bytes.insert(bytes.end(), handshake.peer_id.begin(), handshake.peer_id.end());
	return bytes;
}

PeerHandshake decode_handshake(const Byte* data, std::size_t size) {
	if (size != kHandshakeSize) {
		throw PeerProtocolError("handshake must be exactly 68 bytes");
	}
	if (data[0] != static_cast<Byte>(kProtocolStringLength) ||
		std::memcmp(data + 1, kProtocolString, kProtocolStringLength) != 0) {
		throw PeerProtocolError("invalid BitTorrent protocol identifier");
	}

	PeerHandshake handshake;
	std::size_t offset = 1 + kProtocolStringLength;
	std::memcpy(handshake.reserved.data(), data + offset, handshake.reserved.size());
	offset += handshake.reserved.size();
	std::memcpy(handshake.info_hash.data(), data + offset, handshake.info_hash.size());
	offset += handshake.info_hash.size();
	std::memcpy(handshake.peer_id.data(), data + offset, handshake.peer_id.size());
	return handshake;
}

PeerHandshake decode_handshake(const Bytes& bytes) {
	return decode_handshake(bytes.data(), bytes.size());
}

Bytes encode_message(const PeerMessage& message, std::uint32_t max_length) {
	if (message.keep_alive) {
		if (!message.payload.empty()) {
			throw PeerProtocolError("keep-alive message cannot have a payload");
		}
		return Bytes(4, 0);
	}

	validate_message_payload(message.id, message.payload.size(), max_length);
	if (message.payload.size() >= std::numeric_limits<std::uint32_t>::max()) {
		throw PeerProtocolError("peer message is too large");
	}
	const auto length = static_cast<std::uint32_t>(message.payload.size() + 1);
	if (length > max_length) {
		throw PeerProtocolError("peer message exceeds configured size limit");
	}

	Bytes frame;
	frame.reserve(static_cast<std::size_t>(length) + 4);
	append_u32(frame, length);
	frame.push_back(message.id);
	frame.insert(frame.end(), message.payload.begin(), message.payload.end());
	return frame;
}

PeerMessage decode_message(const Byte* data, std::size_t size, std::uint32_t max_length) {
	if (size < 4) {
		throw PeerProtocolError("peer message is missing its length prefix");
	}

	const auto length = read_u32(data, 0);
	if (length > max_length) {
		throw PeerProtocolError("peer message exceeds configured size limit");
	}
	if (size != static_cast<std::size_t>(length) + 4) {
		throw PeerProtocolError("peer message length prefix does not match frame size");
	}
	if (length == 0) {
		return PeerMessage{true, 0, {}};
	}

	PeerMessage message;
	message.id = data[4];
	message.payload.assign(data + 5, data + size);
	validate_message_payload(message.id, message.payload.size(), max_length);
	return message;
}

PeerMessage decode_message(const Bytes& frame, std::uint32_t max_length) {
	return decode_message(frame.data(), frame.size(), max_length);
}

// Message Constructors
PeerMessage make_keep_alive() {
	return PeerMessage{true, 0, {}};
}

PeerMessage make_choke() {
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Choke), {}};
}

PeerMessage make_unchoke() {
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Unchoke), {}};
}

PeerMessage make_interested() {
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Interested), {}};
}

PeerMessage make_not_interested() {
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::NotInterested), {}};
}

PeerMessage make_have(std::uint32_t piece_index) {
	Bytes payload;
	payload.reserve(4);
	append_u32(payload, piece_index);
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Have), std::move(payload)};
}

PeerMessage make_bitfield(const Bytes& bitfield_bytes) {
	if (bitfield_bytes.empty()) {
		throw PeerProtocolError("bitfield cannot be empty");
	}
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Bitfield), bitfield_bytes};
}

PeerMessage make_bitfield(const std::vector<bool>& bits) {
	if (bits.empty()) {
		throw PeerProtocolError("bitfield bits cannot be empty");
	}
	const std::size_t num_bytes = (bits.size() + 7) / 8;
	Bytes bitfield(num_bytes, 0);
	for (std::size_t i = 0; i < bits.size(); ++i) {
		if (bits[i]) {
			bitfield[i / 8] |= static_cast<Byte>(1 << (7 - (i % 8)));
		}
	}
	return make_bitfield(bitfield);
}

PeerMessage make_request(std::uint32_t index, std::uint32_t begin, std::uint32_t length) {
	if (length == 0) {
		throw PeerProtocolError("request length cannot be zero");
	}
	if (length > kMaxBlockSize) {
		throw PeerProtocolError("request length exceeds maximum allowed block size");
	}
	Bytes payload;
	payload.reserve(12);
	append_u32(payload, index);
	append_u32(payload, begin);
	append_u32(payload, length);
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Request), std::move(payload)};
}

PeerMessage make_piece(std::uint32_t index, std::uint32_t begin, const Byte* data,
					   std::size_t size) {
	if (data == nullptr && size > 0) {
		throw PeerProtocolError("null block data pointer");
	}
	if (size > kMaxBlockSize) {
		throw PeerProtocolError("piece block data exceeds maximum allowed block size");
	}
	Bytes payload;
	payload.reserve(8 + size);
	append_u32(payload, index);
	append_u32(payload, begin);
	if (size > 0) {
		payload.insert(payload.end(), data, data + size);
	}
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Piece), std::move(payload)};
}

PeerMessage make_piece(std::uint32_t index, std::uint32_t begin, const Bytes& data) {
	return make_piece(index, begin, data.data(), data.size());
}

PeerMessage make_cancel(std::uint32_t index, std::uint32_t begin, std::uint32_t length) {
	if (length == 0) {
		throw PeerProtocolError("cancel length cannot be zero");
	}
	Bytes payload;
	payload.reserve(12);
	append_u32(payload, index);
	append_u32(payload, begin);
	append_u32(payload, length);
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Cancel), std::move(payload)};
}

PeerMessage make_port(std::uint16_t port) {
	Bytes payload;
	payload.reserve(2);
	append_u16(payload, port);
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Port), std::move(payload)};
}

PeerMessage make_extended(Byte ext_id, const Bytes& payload) {
	Bytes full_payload;
	full_payload.reserve(1 + payload.size());
	full_payload.push_back(ext_id);
	full_payload.insert(full_payload.end(), payload.begin(), payload.end());
	return PeerMessage{false, static_cast<Byte>(PeerMessageId::Extended),
					   std::move(full_payload)};
}

// Payload Parsers
HavePayload parse_have(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Have) || message.payload.size() != 4) {
		throw PeerProtocolError("invalid have message payload");
	}
	return HavePayload{read_u32(message.payload.data(), 0)};
}

RequestPayload parse_request(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Request) || message.payload.size() != 12) {
		throw PeerProtocolError("invalid request message payload");
	}
	const auto* data = message.payload.data();
	const auto index = read_u32(data, 0);
	const auto begin = read_u32(data, 4);
	const auto length = read_u32(data, 8);
	if (length == 0 || length > kMaxBlockSize) {
		throw PeerProtocolError("invalid request block length");
	}
	return RequestPayload{index, begin, length};
}

PiecePayload parse_piece(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Piece) || message.payload.size() < 9) {
		throw PeerProtocolError("invalid piece message payload");
	}
	const auto* data = message.payload.data();
	const auto index = read_u32(data, 0);
	const auto begin = read_u32(data, 4);
	Bytes block_data(message.payload.begin() + 8, message.payload.end());
	return PiecePayload{index, begin, std::move(block_data)};
}

CancelPayload parse_cancel(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Cancel) || message.payload.size() != 12) {
		throw PeerProtocolError("invalid cancel message payload");
	}
	const auto* data = message.payload.data();
	return CancelPayload{read_u32(data, 0), read_u32(data, 4), read_u32(data, 8)};
}

std::uint16_t parse_port(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Port) || message.payload.size() != 2) {
		throw PeerProtocolError("invalid port message payload");
	}
	return read_u16(message.payload.data(), 0);
}

std::vector<bool> parse_bitfield(const PeerMessage& message, std::size_t num_pieces) {
	if (message.id != static_cast<Byte>(PeerMessageId::Bitfield) || message.payload.empty()) {
		throw PeerProtocolError("invalid bitfield message payload");
	}

	const std::size_t total_bits = (num_pieces > 0) ? num_pieces : (message.payload.size() * 8);
	if (num_pieces > 0) {
		const std::size_t expected_bytes = (num_pieces + 7) / 8;
		if (message.payload.size() != expected_bytes) {
			throw PeerProtocolError("bitfield size does not match piece count");
		}
	}

	std::vector<bool> bits(total_bits, false);
	for (std::size_t i = 0; i < total_bits; ++i) {
		const auto byte_val = message.payload[i / 8];
		const auto bit_mask = static_cast<Byte>(1 << (7 - (i % 8)));
		bits[i] = (byte_val & bit_mask) != 0;
	}

	// Defensive check: if num_pieces specified, spare bits in last byte must be zero
	if (num_pieces > 0 && (num_pieces % 8) != 0) {
		const std::size_t spare_bits = 8 - (num_pieces % 8);
		const auto last_byte = message.payload.back();
		const auto spare_mask = static_cast<Byte>((1 << spare_bits) - 1);
		if ((last_byte & spare_mask) != 0) {
			throw PeerProtocolError("bitfield spare bits must be zero (BEP 3)");
		}
	}

	return bits;
}

}  // namespace peer_protocol
}  // namespace pbl4::network
