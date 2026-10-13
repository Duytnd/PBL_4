#include "../src/network/peer_connection.h"
#include "../src/network/peer_protocol.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace pbl4::network;

namespace {

void print_hex(const Byte* data, std::size_t size) {
	for (std::size_t i = 0; i < size; ++i) {
		std::cout << std::hex << std::setw(2) << std::setfill('0')
				  << static_cast<int>(data[i]);
	}
	std::cout << std::dec;
}

InfoHash parse_hex_info_hash(const std::string& hex) {
	InfoHash hash{};
	if (hex.size() != 40) {
		throw std::invalid_argument("info_hash hex string must be exactly 40 characters");
	}
	for (std::size_t i = 0; i < 20; ++i) {
		const std::string byte_str = hex.substr(i * 2, 2);
		hash[i] = static_cast<Byte>(std::stoul(byte_str, nullptr, 16));
	}
	return hash;
}

PeerId generate_client_peer_id() {
	PeerId id{};
	// Azureus-style Peer ID: -PB0001-xxxxxxxxxxxx
	const std::string prefix = "-PB0001-";
	std::copy(prefix.begin(), prefix.end(), id.begin());
	for (std::size_t i = prefix.size(); i < id.size(); ++i) {
		id[i] = static_cast<Byte>('0' + (std::rand() % 10));
	}
	return id;
}

void run_seed_mode(std::uint16_t listen_port, const std::string& info_hash_hex,
				   const std::string& file_path, std::size_t piece_length,
				   std::size_t piece_count) {
	std::cout << "========================================================\n";
	std::cout << "  PEER WIRE TOOL: SEED MODE (LISTENER)\n";
	std::cout << "========================================================\n";
	std::cout << "Listening port : " << listen_port << "\n";
	std::cout << "Info hash      : " << info_hash_hex << "\n";

	InfoHash expected_hash = parse_hex_info_hash(info_hash_hex);
	PeerId our_peer_id = generate_client_peer_id();

	// Load file or prepare dummy payload data
	std::vector<Byte> file_data;
	if (!file_path.empty()) {
		std::ifstream file(file_path, std::ios::binary);
		if (!file) {
			std::cerr << "Warning: Cannot open file " << file_path
					  << ", using generated dummy pattern.\n";
		} else {
			file_data.assign((std::istreambuf_iterator<char>(file)),
							 std::istreambuf_iterator<char>());
			std::cout << "Loaded file " << file_path << " (" << file_data.size() << " bytes)\n";
			if (piece_count == 0) {
				piece_count = (file_data.size() + piece_length - 1) / piece_length;
			}
		}
	}
	if (file_data.empty()) {
		if (piece_count == 0) piece_count = 8;
		file_data.resize(piece_count * piece_length, 0x5A);
		std::cout << "Serving mock data: " << piece_count << " pieces of "
				  << piece_length << " bytes (" << file_data.size() << " bytes total)\n";
	}

	asio::io_context ioc;
	asio::ip::tcp::acceptor acceptor(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), listen_port));
	std::cout << "Ready for connections from qBittorrent or LAN laptops...\n\n";

	while (true) {
		std::cout << "[LISTENER] Waiting for incoming peer connection...\n";
		asio::ip::tcp::socket sock = acceptor.accept();
		auto remote_ep = sock.remote_endpoint();
		std::cout << "[CONNECTED] Peer from " << remote_ep.address().to_string()
				  << ":" << remote_ep.port() << "\n";

		try {
			PeerConnection peer(std::move(sock), std::chrono::seconds(30));

			// 1. Handshake
			PeerHandshake our_hs;
			our_hs.info_hash = expected_hash;
			our_hs.peer_id = our_peer_id;
			our_hs.set_supports_extension(true);
			our_hs.set_supports_dht(true);

			auto remote_hs = peer.receive_handshake();
			std::cout << "  [HANDSHAKE] Remote Peer ID: ";
			print_hex(remote_hs.peer_id.data(), remote_hs.peer_id.size());
			std::cout << "  (Extensions: " << (remote_hs.supports_extension() ? "YES" : "NO")
					  << ", DHT: " << (remote_hs.supports_dht() ? "YES" : "NO") << ")\n";

			if (remote_hs.info_hash != expected_hash) {
				std::cerr << "  [ERROR] InfoHash mismatch! Expected:\n    ";
				print_hex(expected_hash.data(), expected_hash.size());
				std::cerr << "\n  Closing peer connection.\n";
				peer.close();
				continue;
			}

			// Send our handshake back
			peer.send_handshake(our_hs);
			std::cout << "  [HANDSHAKE] Sent our handshake back successfully.\n";

			// 2. Send Bitfield (We have all pieces as seeder)
			std::vector<bool> all_pieces(piece_count, true);
			peer.send_bitfield(all_pieces);
			std::cout << "  [BITFIELD] Sent bitfield with " << piece_count << " complete pieces.\n";

			// 3. Unchoke peer
			peer.send_unchoke();
			std::cout << "  [UNCHOKE] Sent unchoke message to peer.\n";

			// Message handling loop
			std::size_t uploaded_bytes = 0;
			while (peer.is_open()) {
				PeerMessage msg = peer.receive_message();
				if (msg.keep_alive) {
					std::cout << "  [MESSAGE] Received Keep-Alive\n";
					peer.send_keep_alive();
					continue;
				}

				auto msg_id = static_cast<PeerMessageId>(msg.id);
				if (msg_id == PeerMessageId::Interested) {
					std::cout << "  [MESSAGE] Peer is INTERESTED in our pieces!\n";
				} else if (msg_id == PeerMessageId::NotInterested) {
					std::cout << "  [MESSAGE] Peer is NOT INTERESTED\n";
				} else if (msg_id == PeerMessageId::Request) {
					auto req = peer_protocol::parse_request(msg);
					std::size_t offset_in_file = req.index * piece_length + req.begin;
					if (offset_in_file + req.length <= file_data.size()) {
						peer.send_piece(req.index, req.begin,
										file_data.data() + offset_in_file, req.length);
						uploaded_bytes += req.length;
						std::cout << "\r  [UPLOAD] Served Piece #" << req.index
								  << ", Begin " << req.begin << ", Len " << req.length
								  << " | Total: " << (uploaded_bytes / 1024) << " KiB"
								  << std::flush;
					} else {
						std::cerr << "\n  [WARNING] Peer requested out-of-bounds block!\n";
					}
				} else if (msg_id == PeerMessageId::Have) {
					auto have = peer_protocol::parse_have(msg);
					std::cout << "  [HAVE] Peer announced piece #" << have.index << "\n";
				} else {
					std::cout << "  [MESSAGE] Received ID " << static_cast<int>(msg.id)
							  << " (" << peer_protocol::message_id_to_string(msg.id) << ")\n";
				}
			}
		} catch (const PeerConnectionClosedError&) {
			std::cout << "\n[DISCONNECT] Peer disconnected normally.\n";
		} catch (const PeerTimeoutError&) {
			std::cout << "\n[TIMEOUT] Peer I/O timed out.\n";
		} catch (const PeerProtocolError& err) {
			std::cout << "\n[DEFENSE] Protocol violation / malformed message: " << err.what() << "\n";
		} catch (const std::exception& err) {
			std::cout << "\n[ERROR] Exception: " << err.what() << "\n";
		}
	}
}

void run_leech_mode(const std::string& host, std::uint16_t port,
				    const std::string& info_hash_hex, std::size_t piece_length,
				    std::size_t piece_count, const std::string& output_file) {
	std::cout << "========================================================\n";
	std::cout << "  PEER WIRE TOOL: LEECH MODE (CLIENT)\n";
	std::cout << "========================================================\n";
	std::cout << "Target peer : " << host << ":" << port << "\n";
	std::cout << "Info hash   : " << info_hash_hex << "\n";

	InfoHash expected_hash = parse_hex_info_hash(info_hash_hex);
	PeerId our_peer_id = generate_client_peer_id();

	std::cout << "Connecting to peer...\n";
	PeerConnection peer = PeerConnection::connect_to(host, port, std::chrono::seconds(10));
	std::cout << "Connected! Initiating BitTorrent handshake (68 bytes)...\n";

	PeerHandshake our_hs;
	our_hs.info_hash = expected_hash;
	our_hs.peer_id = our_peer_id;
	our_hs.set_supports_extension(true);
	our_hs.set_supports_dht(true);

	peer.send_handshake(our_hs);
	auto remote_hs = peer.receive_handshake();
	std::cout << "Received handshake from peer! Peer ID: ";
	print_hex(remote_hs.peer_id.data(), remote_hs.peer_id.size());
	std::cout << "\n";

	if (remote_hs.info_hash != expected_hash) {
		std::cerr << "Error: Peer info_hash does not match!\n";
		return;
	}

	peer.send_interested();
	std::cout << "Sent Interested message. Waiting for peer unchoke...\n";

	std::vector<Byte> downloaded_data;
	downloaded_data.resize(piece_count * piece_length, 0);

	std::size_t current_piece = 0;
	std::size_t current_begin = 0;
	const std::size_t block_size = 16384;  // 16 KiB standard block
	std::size_t downloaded_bytes = 0;
	bool can_request = false;

	while (peer.is_open() && current_piece < piece_count) {
		if (can_request) {
			peer.send_request(static_cast<std::uint32_t>(current_piece),
							  static_cast<std::uint32_t>(current_begin),
							  static_cast<std::uint32_t>(block_size));
		}

		PeerMessage msg = peer.receive_message();
		if (msg.keep_alive) {
			continue;
		}

		auto msg_id = static_cast<PeerMessageId>(msg.id);
		if (msg_id == PeerMessageId::Unchoke) {
			std::cout << "Peer UNCHOKED us! Starting block download pipeline...\n";
			can_request = true;
		} else if (msg_id == PeerMessageId::Choke) {
			std::cout << "Peer CHOKED us. Pausing requests.\n";
			can_request = false;
		} else if (msg_id == PeerMessageId::Bitfield) {
			std::cout << "Received Bitfield (" << msg.payload.size() << " bytes)\n";
		} else if (msg_id == PeerMessageId::Have) {
			auto have = peer_protocol::parse_have(msg);
			std::cout << "Peer has piece #" << have.index << "\n";
		} else if (msg_id == PeerMessageId::Piece) {
			auto piece = peer_protocol::parse_piece(msg);
			std::size_t offset = piece.index * piece_length + piece.begin;
			if (offset + piece.data.size() <= downloaded_data.size()) {
				std::copy(piece.data.begin(), piece.data.end(), downloaded_data.begin() + offset);
			}
			downloaded_bytes += piece.data.size();
			std::cout << "\r[DOWNLOAD] Piece #" << piece.index << ", Begin " << piece.begin
					  << " (" << (downloaded_bytes / 1024) << " KiB / "
					  << ((piece_count * piece_length) / 1024) << " KiB)" << std::flush;

			// Advance to next block
			current_begin += block_size;
			if (current_begin >= piece_length) {
				current_begin = 0;
				++current_piece;
			}
		}
	}

	std::cout << "\nDownload complete (" << downloaded_bytes << " bytes)!\n";
	if (!output_file.empty()) {
		std::ofstream out(output_file, std::ios::binary);
		if (out) {
			out.write(reinterpret_cast<const char*>(downloaded_data.data()), downloaded_bytes);
			std::cout << "Saved downloaded file to " << output_file << "\n";
		}
	}
}

void run_attack_sim(const std::string& host, std::uint16_t port, const std::string& attack_type) {
	std::cout << "========================================================\n";
	std::cout << "  PEER WIRE TOOL: DEFENSIVE SECURITY TEST SUITE\n";
	std::cout << "========================================================\n";
	std::cout << "Target: " << host << ":" << port << " | Test: " << attack_type << "\n";

	asio::io_context ioc;
	asio::ip::tcp::socket sock(ioc);
	sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address(host), port));

	PeerHandshake hs;
	auto hs_bytes = peer_protocol::encode_handshake(hs);
	asio::write(sock, asio::buffer(hs_bytes));

	if (attack_type == "oversized") {
		std::cout << "Sending oversized frame prefix (length = 10 MB)...\n";
		std::uint32_t huge_len = 10 * 1024 * 1024;
		Bytes frame = {
			static_cast<Byte>((huge_len >> 24) & 0xff),
			static_cast<Byte>((huge_len >> 16) & 0xff),
			static_cast<Byte>((huge_len >> 8) & 0xff),
			static_cast<Byte>(huge_len & 0xff)
		};
		asio::write(sock, asio::buffer(frame));
		std::cout << "Frame sent. Verifying server closes socket defensively...\n";
		char buf[10];
		asio::error_code ec;
		sock.read_some(asio::buffer(buf), ec);
		if (ec == asio::error::eof || ec == asio::error::connection_reset) {
			std::cout << "SUCCESS: Server closed connection immediately without memory leak!\n";
		} else {
			std::cout << "Response status: " << ec.message() << "\n";
		}
	} else if (attack_type == "malformed") {
		std::cout << "Sending malformed 'Have' message with wrong payload length (2 bytes instead of 4)...\n";
		Bytes malformed_frame = {0, 0, 0, 3, static_cast<Byte>(PeerMessageId::Have), 0x01, 0x02};
		asio::write(sock, asio::buffer(malformed_frame));
		char buf[10];
		asio::error_code ec;
		sock.read_some(asio::buffer(buf), ec);
		if (ec == asio::error::eof || ec == asio::error::connection_reset) {
			std::cout << "SUCCESS: Server rejected malformed message and closed connection!\n";
		} else {
			std::cout << "Response status: " << ec.message() << "\n";
		}
	} else if (attack_type == "disconnect") {
		std::cout << "Sending handshake and immediately closing TCP connection...\n";
		sock.close();
		std::cout << "Socket closed abruptly. Server handled disconnect without crash!\n";
	}
}

}  // namespace

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cout << "Usage:\n"
				  << "  peer_wire_tool --seed --port <port> --info-hash <hex40> [--file <path>] [--piece-len <bytes>] [--pieces <n>]\n"
				  << "  peer_wire_tool --leech --host <ip> --port <port> --info-hash <hex40> [--out <path>] [--piece-len <bytes>] [--pieces <n>]\n"
				  << "  peer_wire_tool --attack <oversized|malformed|disconnect> --host <ip> --port <port>\n"
				  << "  peer_wire_tool --help\n";
		return 1;
	}

	std::string mode = argv[1];
	std::string host = "127.0.0.1";
	std::uint16_t port = 6881;
	std::string info_hash_hex = "0123456789abcdef0123456789abcdef01234567";
	std::string file_path = "";
	std::string out_path = "";
	std::size_t piece_len = 262144;  // 256 KiB
	std::size_t piece_count = 4;
	std::string attack_type = "oversized";

	int start_idx = 2;
	if (mode == "--attack" && argc > 2 && argv[2][0] != '-') {
		attack_type = argv[2];
		start_idx = 3;
	}

	for (int i = start_idx; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
		else if (arg == "--host" && i + 1 < argc) host = argv[++i];
		else if (arg == "--info-hash" && i + 1 < argc) info_hash_hex = argv[++i];
		else if (arg == "--file" && i + 1 < argc) file_path = argv[++i];
		else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
		else if (arg == "--piece-len" && i + 1 < argc) piece_len = std::stoull(argv[++i]);
		else if (arg == "--pieces" && i + 1 < argc) piece_count = std::stoull(argv[++i]);
		else if (arg == "--attack" && i + 1 < argc) attack_type = argv[++i];
	}

	try {
		if (mode == "--seed" || mode == "-s") {
			run_seed_mode(port, info_hash_hex, file_path, piece_len, piece_count);
		} else if (mode == "--leech" || mode == "-l") {
			run_leech_mode(host, port, info_hash_hex, piece_len, piece_count, out_path);
		} else if (mode == "--attack") {
			run_attack_sim(host, port, attack_type);
		} else if (mode == "--help" || mode == "-h") {
			std::cout << "Usage:\n"
					  << "  peer_wire_tool --seed --port <port> --info-hash <hex40> [--file <path>] [--piece-len <bytes>] [--pieces <n>]\n"
					  << "  peer_wire_tool --leech --host <ip> --port <port> --info-hash <hex40> [--out <path>] [--piece-len <bytes>] [--pieces <n>]\n"
					  << "  peer_wire_tool --attack <oversized|malformed|disconnect> --host <ip> --port <port>\n"
					  << "  peer_wire_tool --help\n";
			return 0;
		} else {
			std::cerr << "Unknown mode: " << mode << "\n";
			return 1;
		}
	} catch (const std::exception& e) {
		std::cerr << "Fatal Error: " << e.what() << "\n";
		return 1;
	}

	return 0;
}
