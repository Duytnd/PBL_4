#include "peer_connection.h"

#include <algorithm>
#include <system_error>

namespace pbl4::network {

PeerConnection::PeerConnection(asio::ip::tcp::socket socket,
							   std::chrono::milliseconds io_timeout)
	: socket_(std::move(socket)), io_timeout_(io_timeout) {
	if (io_timeout_.count() < 0) {
		throw std::invalid_argument("I/O timeout cannot be negative");
	}
	update_endpoint_info();
}

PeerConnection::~PeerConnection() {
	close();
}

PeerConnection::PeerConnection(PeerConnection&& other) noexcept
	: owned_ioc_(std::move(other.owned_ioc_)),
	  socket_(std::move(other.socket_)),
	  io_timeout_(other.io_timeout_),
	  am_choking_(other.am_choking_),
	  am_interested_(other.am_interested_),
	  peer_choking_(other.peer_choking_),
	  peer_interested_(other.peer_interested_),
	  handshake_completed_(other.handshake_completed_),
	  remote_handshake_(other.remote_handshake_),
	  remote_endpoint_str_(std::move(other.remote_endpoint_str_)),
	  remote_address_str_(std::move(other.remote_address_str_)),
	  remote_port_(other.remote_port_) {
	other.handshake_completed_ = false;
	other.remote_port_ = 0;
}

PeerConnection& PeerConnection::operator=(PeerConnection&& other) noexcept {
	if (this != &other) {
		close();
		owned_ioc_ = std::move(other.owned_ioc_);
		socket_ = std::move(other.socket_);
		io_timeout_ = other.io_timeout_;
		am_choking_ = other.am_choking_;
		am_interested_ = other.am_interested_;
		peer_choking_ = other.peer_choking_;
		peer_interested_ = other.peer_interested_;
		handshake_completed_ = other.handshake_completed_;
		remote_handshake_ = other.remote_handshake_;
		remote_endpoint_str_ = std::move(other.remote_endpoint_str_);
		remote_address_str_ = std::move(other.remote_address_str_);
		remote_port_ = other.remote_port_;

		other.handshake_completed_ = false;
		other.remote_port_ = 0;
	}
	return *this;
}

PeerConnection PeerConnection::connect_to(const std::string& host, std::uint16_t port,
										  std::chrono::milliseconds io_timeout) {
	if (io_timeout.count() < 0) {
		throw std::invalid_argument("I/O timeout cannot be negative");
	}

	auto ioc = std::make_unique<asio::io_context>();
	asio::ip::tcp::resolver resolver(*ioc);
	asio::ip::tcp::socket socket(*ioc);

	asio::steady_timer timer(*ioc);
	timer.expires_after(io_timeout);
	bool timed_out = false;
	asio::error_code connect_ec;

	timer.async_wait([&](const asio::error_code& ec) {
		if (!ec) {
			timed_out = true;
			asio::error_code ignored;
			socket.close(ignored);
			resolver.cancel();
		}
	});

	asio::error_code resolve_ec;
	auto endpoints = resolver.resolve(host, std::to_string(port), resolve_ec);
	if (resolve_ec) {
		timer.cancel();
		throw std::runtime_error("failed to resolve host " + host + ": " + resolve_ec.message());
	}

	asio::async_connect(socket, endpoints,
						[&](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
							connect_ec = ec;
							timer.cancel();
						});

	ioc->restart();
	ioc->run();

	if (timed_out) {
		throw PeerTimeoutError("connection to " + host + ":" + std::to_string(port) +
							   " timed out");
	}
	if (connect_ec) {
		throw std::runtime_error("connect to " + host + ":" + std::to_string(port) +
								 " failed: " + connect_ec.message());
	}

	PeerConnection connection(std::move(socket), io_timeout);
	connection.owned_ioc_ = std::move(ioc);
	return connection;
}

void PeerConnection::send_handshake(const PeerHandshake& handshake) {
	const auto bytes = peer_protocol::encode_handshake(handshake);
	try {
		send_all(bytes.data(), bytes.size());
	} catch (...) {
		close();
		throw;
	}
}

PeerHandshake PeerConnection::receive_handshake() {
	Bytes bytes(kHandshakeSize);
	try {
		receive_exact(bytes.data(), bytes.size());
		auto handshake = peer_protocol::decode_handshake(bytes);
		remote_handshake_ = handshake;
		handshake_completed_ = true;
		return handshake;
	} catch (...) {
		close();
		throw;
	}
}

void PeerConnection::send_message(const PeerMessage& message) {
	const auto frame = peer_protocol::encode_message(message);
	try {
		send_all(frame.data(), frame.size());
	} catch (...) {
		close();
		throw;
	}
}

PeerMessage PeerConnection::receive_message(std::uint32_t max_length) {
	std::array<Byte, 4> length_bytes{};
	try {
		receive_exact(length_bytes.data(), length_bytes.size());
		const auto length = (static_cast<std::uint32_t>(length_bytes[0]) << 24) |
							(static_cast<std::uint32_t>(length_bytes[1]) << 16) |
							(static_cast<std::uint32_t>(length_bytes[2]) << 8) |
							static_cast<std::uint32_t>(length_bytes[3]);

		if (length > max_length) {
			throw PeerProtocolError("peer message exceeds configured size limit");
		}

		Bytes frame(length_bytes.begin(), length_bytes.end());
		frame.resize(static_cast<std::size_t>(length) + length_bytes.size());
		if (length != 0) {
			receive_exact(frame.data() + length_bytes.size(), length);
		}

		auto message = peer_protocol::decode_message(frame, max_length);

		// Automatically track peer's choke / interested status
		if (!message.keep_alive) {
			if (message.id == static_cast<Byte>(PeerMessageId::Choke)) {
				peer_choking_ = true;
			} else if (message.id == static_cast<Byte>(PeerMessageId::Unchoke)) {
				peer_choking_ = false;
			} else if (message.id == static_cast<Byte>(PeerMessageId::Interested)) {
				peer_interested_ = true;
			} else if (message.id == static_cast<Byte>(PeerMessageId::NotInterested)) {
				peer_interested_ = false;
			}
		}

		return message;
	} catch (...) {
		close();
		throw;
	}
}

void PeerConnection::send_keep_alive() {
	send_message(peer_protocol::make_keep_alive());
}

void PeerConnection::send_choke() {
	send_message(peer_protocol::make_choke());
	am_choking_ = true;
}

void PeerConnection::send_unchoke() {
	send_message(peer_protocol::make_unchoke());
	am_choking_ = false;
}

void PeerConnection::send_interested() {
	send_message(peer_protocol::make_interested());
	am_interested_ = true;
}

void PeerConnection::send_not_interested() {
	send_message(peer_protocol::make_not_interested());
	am_interested_ = false;
}

void PeerConnection::send_have(std::uint32_t piece_index) {
	send_message(peer_protocol::make_have(piece_index));
}

void PeerConnection::send_bitfield(const Bytes& bitfield_bytes) {
	send_message(peer_protocol::make_bitfield(bitfield_bytes));
}

void PeerConnection::send_bitfield(const std::vector<bool>& bits) {
	send_message(peer_protocol::make_bitfield(bits));
}

void PeerConnection::send_request(std::uint32_t index, std::uint32_t begin,
								  std::uint32_t length) {
	send_message(peer_protocol::make_request(index, begin, length));
}

void PeerConnection::send_piece(std::uint32_t index, std::uint32_t begin,
								const Bytes& data) {
	send_message(peer_protocol::make_piece(index, begin, data));
}

void PeerConnection::send_piece(std::uint32_t index, std::uint32_t begin,
								const Byte* data, std::size_t size) {
	send_message(peer_protocol::make_piece(index, begin, data, size));
}

void PeerConnection::send_cancel(std::uint32_t index, std::uint32_t begin,
								 std::uint32_t length) {
	send_message(peer_protocol::make_cancel(index, begin, length));
}

void PeerConnection::send_port(std::uint16_t port) {
	send_message(peer_protocol::make_port(port));
}

void PeerConnection::close() noexcept {
	if (socket_.is_open()) {
		asio::error_code ec;
		socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
		socket_.close(ec);
	}
}

bool PeerConnection::is_open() const noexcept {
	return socket_.is_open();
}

PeerConnection::NativeSocket PeerConnection::native_handle() noexcept {
	if (socket_.is_open()) {
#ifdef _WIN32
		return static_cast<NativeSocket>(static_cast<SOCKET>(socket_.native_handle()));
#else
		return static_cast<NativeSocket>(socket_.native_handle());
#endif
	}
	return kInvalidSocket;
}

asio::ip::tcp::socket& PeerConnection::socket() noexcept {
	return socket_;
}

const asio::ip::tcp::socket& PeerConnection::socket() const noexcept {
	return socket_;
}

void PeerConnection::set_io_timeout(std::chrono::milliseconds timeout) noexcept {
	io_timeout_ = timeout;
}

std::chrono::milliseconds PeerConnection::io_timeout() const noexcept {
	return io_timeout_;
}

void PeerConnection::update_endpoint_info() {
	if (socket_.is_open()) {
		asio::error_code ec;
		auto endpoint = socket_.remote_endpoint(ec);
		if (!ec) {
			remote_address_str_ = endpoint.address().to_string();
			remote_port_ = endpoint.port();
			remote_endpoint_str_ = remote_address_str_ + ":" + std::to_string(remote_port_);
		}
	}
}

void PeerConnection::send_all(const Byte* data, std::size_t size) {
	if (!is_open()) {
		throw PeerConnectionClosedError("connection is closed");
	}
	if (size == 0) {
		return;
	}

	auto& ioc = static_cast<asio::io_context&>(socket_.get_executor().context());
	asio::steady_timer timer(socket_.get_executor());
	timer.expires_after(io_timeout_);
	bool timed_out = false;
	asio::error_code write_ec;

	timer.async_wait([&](const asio::error_code& ec) {
		if (!ec) {
			timed_out = true;
			asio::error_code ignored;
			socket_.cancel(ignored);
		}
	});

	asio::async_write(socket_, asio::buffer(data, size),
					  [&](const asio::error_code& ec, std::size_t) {
						  write_ec = ec;
						  timer.cancel();
					  });

	ioc.restart();
	ioc.run();

	if (timed_out) {
		close();
		throw PeerTimeoutError("I/O timed out while writing");
	}
	if (write_ec == asio::error::eof || write_ec == asio::error::connection_reset) {
		close();
		throw PeerConnectionClosedError("peer closed connection during write");
	}
	if (write_ec) {
		close();
		throw std::runtime_error("write failed: " + write_ec.message());
	}
}

void PeerConnection::receive_exact(Byte* data, std::size_t size) {
	if (!is_open()) {
		throw PeerConnectionClosedError("connection is closed");
	}
	if (size == 0) {
		return;
	}

	auto& ioc = static_cast<asio::io_context&>(socket_.get_executor().context());
	asio::steady_timer timer(socket_.get_executor());
	timer.expires_after(io_timeout_);
	bool timed_out = false;
	asio::error_code read_ec;

	timer.async_wait([&](const asio::error_code& ec) {
		if (!ec) {
			timed_out = true;
			asio::error_code ignored;
			socket_.cancel(ignored);
		}
	});

	asio::async_read(socket_, asio::buffer(data, size),
					 [&](const asio::error_code& ec, std::size_t) {
						 read_ec = ec;
						 timer.cancel();
					 });

	ioc.restart();
	ioc.run();

	if (timed_out) {
		close();
		throw PeerTimeoutError("I/O timed out while reading");
	}
	if (read_ec == asio::error::eof || read_ec == asio::error::connection_reset) {
		close();
		throw PeerConnectionClosedError("peer closed the connection");
	}
	if (read_ec) {
		close();
		throw std::runtime_error("read failed: " + read_ec.message());
	}
}

}  // namespace pbl4::network
