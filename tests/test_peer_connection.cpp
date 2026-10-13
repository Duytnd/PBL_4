#include "../src/network/peer_connection.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace pbl4::network;

void test_network_handshake_and_piece_exchange() {
	std::cout << "[TEST] Network handshake and 16 KiB piece exchange over loopback... ";

	asio::io_context server_ioc;
	asio::ip::tcp::acceptor acceptor(server_ioc,
									 asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
	const auto port = acceptor.local_endpoint().port();

	PeerHandshake alice_handshake;
	alice_handshake.info_hash.fill(0x11);
	alice_handshake.peer_id.fill(0xAA);
	alice_handshake.set_supports_extension(true);

	PeerHandshake bob_handshake;
	bob_handshake.info_hash.fill(0x11);
	bob_handshake.peer_id.fill(0xBB);
	bob_handshake.set_supports_extension(true);

	// Client (Bob) connects in another thread or synchronously
	std::thread client_thread([&]() {
		auto bob = PeerConnection::connect_to("127.0.0.1", port, std::chrono::seconds(5));
		bob.send_handshake(bob_handshake);
		const auto received_alice = bob.receive_handshake();
		assert(received_alice.info_hash == alice_handshake.info_hash);
		assert(received_alice.peer_id == alice_handshake.peer_id);
		assert(received_alice.supports_extension());

		// Bob receives bitfield from Alice
		const auto bitfield_msg = bob.receive_message();
		assert(bitfield_msg.id == static_cast<Byte>(PeerMessageId::Bitfield));
		const auto bits = peer_protocol::parse_bitfield(bitfield_msg, 8);
		assert(bits[0] == true);

		// Bob expresses interest
		bob.send_interested();
		assert(bob.am_interested());

		// Bob waits for unchoke
		const auto unchoke_msg = bob.receive_message();
		assert(unchoke_msg.id == static_cast<Byte>(PeerMessageId::Unchoke));
		assert(!bob.peer_choking());

		// Bob sends a 16 KiB block request
		bob.send_request(0, 0, 16384);

		// Bob receives 16 KiB piece block
		const auto piece_msg = bob.receive_message();
		assert(piece_msg.id == static_cast<Byte>(PeerMessageId::Piece));
		const auto piece = peer_protocol::parse_piece(piece_msg);
		assert(piece.index == 0);
		assert(piece.begin == 0);
		assert(piece.data.size() == 16384);
		assert(piece.data[0] == 0x42);
		assert(piece.data[16383] == 0x42);

		// Bob sends keep-alive and closes
		bob.send_keep_alive();
		bob.close();
	});

	// Server (Alice) accepts Bob
	auto alice_sock = acceptor.accept();
	PeerConnection alice(std::move(alice_sock), std::chrono::seconds(5));

	const auto received_bob = alice.receive_handshake();
	assert(received_bob.info_hash == bob_handshake.info_hash);
	assert(received_bob.peer_id == bob_handshake.peer_id);
	alice.send_handshake(alice_handshake);

	// Alice sends Bitfield (1 piece available)
	std::vector<bool> alice_pieces = {true, false, false, false, false, false, false, false};
	alice.send_bitfield(alice_pieces);

	// Alice receives Interested
	const auto interested_msg = alice.receive_message();
	assert(interested_msg.id == static_cast<Byte>(PeerMessageId::Interested));
	assert(alice.peer_interested());

	// Alice unchokes Bob
	alice.send_unchoke();
	assert(!alice.am_choking());

	// Alice receives Request
	const auto req_msg = alice.receive_message();
	assert(req_msg.id == static_cast<Byte>(PeerMessageId::Request));
	const auto req = peer_protocol::parse_request(req_msg);
	assert(req.index == 0);
	assert(req.begin == 0);
	assert(req.length == 16384);

	// Alice fulfills Request with 16 KiB dummy block data
	Bytes piece_data(16384, 0x42);
	alice.send_piece(req.index, req.begin, piece_data);

	// Alice receives keep-alive
	const auto keep_alive = alice.receive_message();
	assert(keep_alive.keep_alive);

	alice.close();
	client_thread.join();

	std::cout << "PASSED\n";
}

void test_peer_abrupt_disconnect() {
	std::cout << "[TEST] Peer abrupt disconnect handling... ";

	asio::io_context server_ioc;
	asio::ip::tcp::acceptor acceptor(server_ioc,
									 asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
	const auto port = acceptor.local_endpoint().port();

	std::thread client_thread([&]() {
		auto conn = PeerConnection::connect_to("127.0.0.1", port, std::chrono::seconds(2));
		// Close socket immediately without proper handshake
		conn.close();
	});

	auto accepted_sock = acceptor.accept();
	PeerConnection server_conn(std::move(accepted_sock), std::chrono::seconds(2));

	bool caught_disconnect = false;
	try {
		(void)server_conn.receive_handshake();
	} catch (const PeerConnectionClosedError&) {
		caught_disconnect = true;
	} catch (const std::exception& e) {
		// Could also be general runtime_error depending on OS socket reset
		caught_disconnect = true;
	}
	assert(caught_disconnect);
	assert(!server_conn.is_open());

	client_thread.join();
	std::cout << "PASSED\n";
}

void test_oversized_and_malformed_messages_over_network() {
	std::cout << "[TEST] Oversized and malformed message rejection over network... ";

	asio::io_context server_ioc;
	asio::ip::tcp::acceptor acceptor(server_ioc,
									 asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
	const auto port = acceptor.local_endpoint().port();

	// Test 1: Oversized frame rejection (> 128 KiB)
	std::thread attacker_thread([&]() {
		asio::io_context client_ioc;
		asio::ip::tcp::socket raw_sock(client_ioc);
		raw_sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));

		// Send handshake first
		PeerHandshake hs;
		auto hs_bytes = peer_protocol::encode_handshake(hs);
		asio::write(raw_sock, asio::buffer(hs_bytes));

		// Now send an oversized message header claiming 512 KiB (524288 bytes)
		std::uint32_t malicious_length = 524288;
		Bytes evil_header;
		evil_header.push_back(static_cast<Byte>((malicious_length >> 24) & 0xff));
		evil_header.push_back(static_cast<Byte>((malicious_length >> 16) & 0xff));
		evil_header.push_back(static_cast<Byte>((malicious_length >> 8) & 0xff));
		evil_header.push_back(static_cast<Byte>(malicious_length & 0xff));
		asio::write(raw_sock, asio::buffer(evil_header));

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		raw_sock.close();
	});

	auto victim_sock = acceptor.accept();
	PeerConnection victim(std::move(victim_sock), std::chrono::seconds(2));
	(void)victim.receive_handshake();

	bool caught_oversized = false;
	try {
		(void)victim.receive_message(128 * 1024);
	} catch (const PeerProtocolError& err) {
		caught_oversized = true;
	}
	assert(caught_oversized);
	assert(!victim.is_open());

	attacker_thread.join();

	// Test 2: Malformed payload (Have with only 2 bytes)
	std::thread bad_payload_thread([&]() {
		asio::io_context client_ioc;
		asio::ip::tcp::socket raw_sock(client_ioc);
		raw_sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));

		PeerHandshake hs;
		auto hs_bytes = peer_protocol::encode_handshake(hs);
		asio::write(raw_sock, asio::buffer(hs_bytes));

		// Send frame: length=3, id=4 (Have), payload=2 bytes instead of 4
		Bytes malformed_frame = {0, 0, 0, 3, static_cast<Byte>(PeerMessageId::Have), 0x01, 0x02};
		asio::write(raw_sock, asio::buffer(malformed_frame));

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		raw_sock.close();
	});

	auto victim_sock2 = acceptor.accept();
	PeerConnection victim2(std::move(victim_sock2), std::chrono::seconds(2));
	(void)victim2.receive_handshake();

	bool caught_malformed = false;
	try {
		(void)victim2.receive_message();
	} catch (const PeerProtocolError& err) {
		caught_malformed = true;
	}
	assert(caught_malformed);
	assert(!victim2.is_open());

	bad_payload_thread.join();
	std::cout << "PASSED\n";
}

void test_network_timeout() {
	std::cout << "[TEST] Network I/O timeout detection... ";

	asio::io_context server_ioc;
	asio::ip::tcp::acceptor acceptor(server_ioc,
									 asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
	const auto port = acceptor.local_endpoint().port();

	std::thread silent_peer([&]() {
		asio::io_context client_ioc;
		asio::ip::tcp::socket raw_sock(client_ioc);
		raw_sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
		// Stay silent for 300ms without sending anything
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		raw_sock.close();
	});

	auto sock = acceptor.accept();
	// Set a very short timeout of 100ms
	PeerConnection conn(std::move(sock), std::chrono::milliseconds(100));

	bool timed_out = false;
	try {
		(void)conn.receive_handshake();
	} catch (const PeerTimeoutError&) {
		timed_out = true;
	}
	assert(timed_out);
	assert(!conn.is_open());

	silent_peer.join();
	std::cout << "PASSED\n";
}

int main() {
	std::cout << "========================================\n";
	std::cout << " RUNNING PEER CONNECTION INTEGRATION TESTS\n";
	std::cout << "========================================\n";

	test_network_handshake_and_piece_exchange();
	test_peer_abrupt_disconnect();
	test_oversized_and_malformed_messages_over_network();
	test_network_timeout();

	std::cout << "========================================\n";
	std::cout << " ALL PEER CONNECTION TESTS PASSED!\n";
	std::cout << "========================================\n";
	return 0;
}

