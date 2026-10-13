#pragma once

#include "peer_protocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

namespace pbl4::network {

class PeerConnectionClosedError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class PeerTimeoutError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class PeerConnection {
public:
	using NativeSocket = std::intptr_t;
	static constexpr NativeSocket kInvalidSocket = static_cast<NativeSocket>(-1);

	// Construct from an accepted or existing Asio socket
	explicit PeerConnection(
		asio::ip::tcp::socket socket,
		std::chrono::milliseconds io_timeout = std::chrono::seconds(30));

	~PeerConnection();

	PeerConnection(const PeerConnection&) = delete;
	PeerConnection& operator=(const PeerConnection&) = delete;

	PeerConnection(PeerConnection&& other) noexcept;
	PeerConnection& operator=(PeerConnection&& other) noexcept;

	// Connect outbound to a remote host and port
	static PeerConnection connect_to(
		const std::string& host,
		std::uint16_t port,
		std::chrono::milliseconds io_timeout = std::chrono::seconds(30));

	// Wire operations
	void send_handshake(const PeerHandshake& handshake);
	PeerHandshake receive_handshake();

	void send_message(const PeerMessage& message);
	PeerMessage receive_message(std::uint32_t max_length = kMaxPeerMessageLength);

	// BEP 3 Convenience senders
	void send_keep_alive();
	void send_choke();
	void send_unchoke();
	void send_interested();
	void send_not_interested();
	void send_have(std::uint32_t piece_index);
	void send_bitfield(const Bytes& bitfield_bytes);
	void send_bitfield(const std::vector<bool>& bits);
	void send_request(std::uint32_t index, std::uint32_t begin,
					  std::uint32_t length = kStandardBlockSize);
	void send_piece(std::uint32_t index, std::uint32_t begin, const Bytes& data);
	void send_piece(std::uint32_t index, std::uint32_t begin, const Byte* data,
					std::size_t size);
	void send_cancel(std::uint32_t index, std::uint32_t begin, std::uint32_t length);
	void send_port(std::uint16_t port);

	// Lifecycle & native access
	void close() noexcept;
	bool is_open() const noexcept;
	NativeSocket native_handle() noexcept;

	asio::ip::tcp::socket& socket() noexcept;
	const asio::ip::tcp::socket& socket() const noexcept;

	// Timeout configuration
	void set_io_timeout(std::chrono::milliseconds timeout) noexcept;
	std::chrono::milliseconds io_timeout() const noexcept;

	// BEP 3 Peer Connection State
	bool am_choking() const noexcept { return am_choking_; }
	bool am_interested() const noexcept { return am_interested_; }
	bool peer_choking() const noexcept { return peer_choking_; }
	bool peer_interested() const noexcept { return peer_interested_; }

	void set_am_choking(bool v) noexcept { am_choking_ = v; }
	void set_am_interested(bool v) noexcept { am_interested_ = v; }
	void set_peer_choking(bool v) noexcept { peer_choking_ = v; }
	void set_peer_interested(bool v) noexcept { peer_interested_ = v; }

	bool handshake_completed() const noexcept { return handshake_completed_; }
	const PeerHandshake& remote_handshake() const noexcept { return remote_handshake_; }
	const InfoHash& remote_info_hash() const noexcept { return remote_handshake_.info_hash; }
	const PeerId& remote_peer_id() const noexcept { return remote_handshake_.peer_id; }

	const std::string& remote_endpoint() const noexcept { return remote_endpoint_str_; }
	const std::string& remote_address() const noexcept { return remote_address_str_; }
	std::uint16_t remote_port() const noexcept { return remote_port_; }

private:
	std::unique_ptr<asio::io_context> owned_ioc_;
	asio::ip::tcp::socket socket_;
	std::chrono::milliseconds io_timeout_{30000};

	// State
	bool am_choking_ = true;
	bool am_interested_ = false;
	bool peer_choking_ = true;
	bool peer_interested_ = false;
	bool handshake_completed_ = false;
	PeerHandshake remote_handshake_{};

	std::string remote_endpoint_str_{};
	std::string remote_address_str_{};
	std::uint16_t remote_port_ = 0;

	void send_all(const Byte* data, std::size_t size);
	void receive_exact(Byte* data, std::size_t size);
	void update_endpoint_info();
};

}  // namespace pbl4::network
