#include "../src/network/peer_protocol.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace pbl4::network;

void test_handshake_encoding_and_decoding() {
	std::cout << "[TEST] Handshake encoding and decoding... ";
	PeerHandshake handshake;
	for (std::size_t i = 0; i < handshake.reserved.size(); ++i) {
		handshake.reserved[i] = static_cast<Byte>(i + 1);
	}
	for (std::size_t i = 0; i < handshake.info_hash.size(); ++i) {
		handshake.info_hash[i] = static_cast<Byte>(i + 10);
		handshake.peer_id[i] = static_cast<Byte>(i + 50);
	}

	// Verify BEP 10 extension and BEP 5 DHT flag helpers
	handshake.set_supports_extension(false);
	assert(!handshake.supports_extension());
	handshake.set_supports_extension(true);
	assert(handshake.supports_extension());
	assert((handshake.reserved[5] & 0x10) != 0);

	handshake.set_supports_dht(false);
	assert(!handshake.supports_dht());
	handshake.set_supports_dht(true);
	assert(handshake.supports_dht());
	assert((handshake.reserved[7] & 0x01) != 0);

	const auto handshake_bytes = peer_protocol::encode_handshake(handshake);
	assert(handshake_bytes.size() == kHandshakeSize);
	assert(handshake_bytes[0] == 19);

	const auto decoded = peer_protocol::decode_handshake(handshake_bytes);
	assert(decoded.reserved == handshake.reserved);
	assert(decoded.info_hash == handshake.info_hash);
	assert(decoded.peer_id == handshake.peer_id);
	assert(decoded.supports_extension());
	assert(decoded.supports_dht());

	std::cout << "PASSED\n";
}

void test_handshake_defensive_rejections() {
	std::cout << "[TEST] Handshake defensive rejections... ";
	PeerHandshake handshake;
	const auto valid_bytes = peer_protocol::encode_handshake(handshake);

	// Wrong length (too short)
	bool caught = false;
	try {
		Bytes short_bytes(valid_bytes.begin(), valid_bytes.begin() + 60);
		(void)peer_protocol::decode_handshake(short_bytes);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Wrong protocol string length byte
	caught = false;
	try {
		auto bad_pstrlen = valid_bytes;
		bad_pstrlen[0] = 20;
		(void)peer_protocol::decode_handshake(bad_pstrlen);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Corrupted protocol identifier string
	caught = false;
	try {
		auto corrupted = valid_bytes;
		corrupted[1] = 'X';
		(void)peer_protocol::decode_handshake(corrupted);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	std::cout << "PASSED\n";
}

void test_control_messages() {
	std::cout << "[TEST] Control messages (Keep-alive, Choke, Unchoke, Interested, NotInterested)... ";

	// Keep-alive
	const auto keep_alive = peer_protocol::make_keep_alive();
	assert(keep_alive.keep_alive);
	const auto keep_alive_frame = peer_protocol::encode_message(keep_alive);
	assert(keep_alive_frame == Bytes({0, 0, 0, 0}));
	const auto decoded_keep_alive = peer_protocol::decode_message(keep_alive_frame);
	assert(decoded_keep_alive.keep_alive);

	// Choke
	const auto choke = peer_protocol::make_choke();
	const auto choke_frame = peer_protocol::encode_message(choke);
	assert(choke_frame.size() == 5);
	assert(choke_frame[4] == static_cast<Byte>(PeerMessageId::Choke));
	const auto decoded_choke = peer_protocol::decode_message(choke_frame);
	assert(!decoded_choke.keep_alive);
	assert(decoded_choke.id == static_cast<Byte>(PeerMessageId::Choke));
	assert(decoded_choke.payload.empty());

	// Unchoke
	const auto unchoke = peer_protocol::make_unchoke();
	const auto unchoke_frame = peer_protocol::encode_message(unchoke);
	const auto decoded_unchoke = peer_protocol::decode_message(unchoke_frame);
	assert(decoded_unchoke.id == static_cast<Byte>(PeerMessageId::Unchoke));

	// Interested
	const auto interested = peer_protocol::make_interested();
	const auto interested_frame = peer_protocol::encode_message(interested);
	const auto decoded_interested = peer_protocol::decode_message(interested_frame);
	assert(decoded_interested.id == static_cast<Byte>(PeerMessageId::Interested));

	// NotInterested
	const auto not_interested = peer_protocol::make_not_interested();
	const auto not_interested_frame = peer_protocol::encode_message(not_interested);
	const auto decoded_not_interested = peer_protocol::decode_message(not_interested_frame);
	assert(decoded_not_interested.id == static_cast<Byte>(PeerMessageId::NotInterested));

	std::cout << "PASSED\n";
}

void test_have_and_bitfield_messages() {
	std::cout << "[TEST] Have and Bitfield messages... ";

	// Have
	const auto have_msg = peer_protocol::make_have(42);
	const auto have_frame = peer_protocol::encode_message(have_msg);
	assert(have_frame.size() == 9);  // 4 len + 1 id + 4 payload
	const auto decoded_have = peer_protocol::decode_message(have_frame);
	const auto have_parsed = peer_protocol::parse_have(decoded_have);
	assert(have_parsed.index == 42);

	// Bitfield from vector<bool>
	std::vector<bool> pieces = {true, false, true, true, false, false, false, true, true, false};
	const auto bitfield_msg = peer_protocol::make_bitfield(pieces);
	assert(bitfield_msg.payload.size() == 2);  // 10 bits -> 2 bytes
	const auto bitfield_frame = peer_protocol::encode_message(bitfield_msg);
	const auto decoded_bitfield = peer_protocol::decode_message(bitfield_frame);
	const auto parsed_bits = peer_protocol::parse_bitfield(decoded_bitfield, pieces.size());
	assert(parsed_bits == pieces);

	std::cout << "PASSED\n";
}

void test_request_piece_cancel_messages() {
	std::cout << "[TEST] Request, Piece, and Cancel messages... ";

	// Request: index 5, begin 16384, length 16384 (16 KiB standard block)
	const auto req_msg = peer_protocol::make_request(5, 16384, 16384);
	const auto req_frame = peer_protocol::encode_message(req_msg);
	assert(req_frame.size() == 17);  // 4 len + 1 id + 12 payload
	const auto decoded_req = peer_protocol::decode_message(req_frame);
	const auto parsed_req = peer_protocol::parse_request(decoded_req);
	assert(parsed_req.index == 5);
	assert(parsed_req.begin == 16384);
	assert(parsed_req.length == 16384);

	// Piece: 16 KiB block data
	Bytes dummy_data(16384, 0xAB);
	dummy_data[0] = 0x12;
	dummy_data[16383] = 0x34;
	const auto piece_msg = peer_protocol::make_piece(5, 16384, dummy_data);
	const auto piece_frame = peer_protocol::encode_message(piece_msg);
	assert(piece_frame.size() == 4 + 1 + 8 + 16384);
	const auto decoded_piece = peer_protocol::decode_message(piece_frame);
	const auto parsed_piece = peer_protocol::parse_piece(decoded_piece);
	assert(parsed_piece.index == 5);
	assert(parsed_piece.begin == 16384);
	assert(parsed_piece.data == dummy_data);

	// Cancel
	const auto cancel_msg = peer_protocol::make_cancel(5, 16384, 16384);
	const auto cancel_frame = peer_protocol::encode_message(cancel_msg);
	const auto decoded_cancel = peer_protocol::decode_message(cancel_frame);
	const auto parsed_cancel = peer_protocol::parse_cancel(decoded_cancel);
	assert(parsed_cancel.index == 5);
	assert(parsed_cancel.begin == 16384);
	assert(parsed_cancel.length == 16384);

	// Port message
	const auto port_msg = peer_protocol::make_port(6881);
	const auto port_frame = peer_protocol::encode_message(port_msg);
	const auto decoded_port = peer_protocol::decode_message(port_frame);
	assert(peer_protocol::parse_port(decoded_port) == 6881);

	std::cout << "PASSED\n";
}

void test_defensive_message_validation() {
	std::cout << "[TEST] Defensive limits and malformed message validation... ";

	// Oversized message length rejection (> 128 KiB limit)
	bool caught = false;
	try {
		Bytes oversized_frame = {0x00, 0x04, 0x00, 0x00, 0x00};  // claims 262144 bytes
		(void)peer_protocol::decode_message(oversized_frame, 128 * 1024);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Frame length mismatch (header claims 10 bytes, but buffer has 6)
	caught = false;
	try {
		Bytes mismatched = {0, 0, 0, 10, 1, 2};
		(void)peer_protocol::decode_message(mismatched);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Control message with unexpected payload (choke with 2 bytes)
	caught = false;
	try {
		Bytes bad_choke = {0, 0, 0, 3, 0, 0xAA, 0xBB};
		(void)peer_protocol::decode_message(bad_choke);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Have message with wrong payload length (should be 4, got 2)
	caught = false;
	try {
		Bytes bad_have = {0, 0, 0, 3, 4, 0x01, 0x02};
		(void)peer_protocol::decode_message(bad_have);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Request message with wrong payload length (should be 12, got 8)
	caught = false;
	try {
		Bytes bad_req = {0, 0, 0, 9, 6, 0, 0, 0, 1, 0, 0, 0, 0};
		(void)peer_protocol::decode_message(bad_req);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Request with zero block length
	caught = false;
	try {
		(void)peer_protocol::make_request(0, 0, 0);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Request with oversized block length (> 128 KiB)
	caught = false;
	try {
		(void)peer_protocol::make_request(0, 0, 256 * 1024);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	// Bitfield with non-zero spare bits rejection (BEP 3 compliance)
	caught = false;
	try {
		// 9 pieces = 1 byte + 1 bit. Remaining 7 bits in byte 2 must be 0.
		// If last byte has 0x01, it sets invalid spare bits.
		PeerMessage bad_bitfield;
		bad_bitfield.id = static_cast<Byte>(PeerMessageId::Bitfield);
		bad_bitfield.payload = {0xFF, 0x81};  // 0x80 is 9th bit, 0x01 is forbidden spare bit
		(void)peer_protocol::parse_bitfield(bad_bitfield, 9);
	} catch (const PeerProtocolError&) {
		caught = true;
	}
	assert(caught);

	std::cout << "PASSED\n";
}

int main() {
	std::cout << "========================================\n";
	std::cout << " RUNNING PEER PROTOCOL UNIT TESTS\n";
	std::cout << "========================================\n";

	test_handshake_encoding_and_decoding();
	test_handshake_defensive_rejections();
	test_control_messages();
	test_have_and_bitfield_messages();
	test_request_piece_cancel_messages();
	test_defensive_message_validation();

	std::cout << "========================================\n";
	std::cout << " ALL PEER PROTOCOL TESTS PASSED!\n";
	std::cout << "========================================\n";
	return 0;
}
