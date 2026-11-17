#include "network/utp_engine.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <stdexcept>
#include <system_error>

namespace pbl4::network {

UtpEngine::UtpEngine()
    : socket_(ioc_), timeout_timer_(ioc_) {}

UtpEngine::~UtpEngine() {
    stop();
}

void UtpEngine::start() {
    if (running_.load()) {
        return;
    }

    socket_.open(asio::ip::udp::v4());
    socket_.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    local_port_ = socket_.local_endpoint().port();

    context_ = utp_init(2);
    if (!context_) {
        asio::error_code ec;
        socket_.close(ec);
        throw std::runtime_error("libutp failed to create a context");
    }

    utp_context_set_userdata(context_, this);
    utp_set_callback(context_, UTP_SENDTO, &UtpEngine::on_send_to);
    utp_set_callback(context_, UTP_ON_ACCEPT, &UtpEngine::on_accept);
    utp_set_callback(context_, UTP_ON_FIREWALL, &UtpEngine::on_firewall);
    utp_set_callback(context_, UTP_ON_READ, &UtpEngine::on_read);
    utp_set_callback(context_, UTP_ON_ERROR, &UtpEngine::on_error);
    utp_set_callback(context_, UTP_ON_STATE_CHANGE, &UtpEngine::on_state_change);
    utp_set_callback(context_, UTP_GET_READ_BUFFER_SIZE, &UtpEngine::on_read_buffer_size);

    running_.store(true);
    receive_datagram();
    check_timeouts();
    thread_ = std::thread([this]() { ioc_.run(); });
}

void UtpEngine::stop() noexcept {
    if (!running_.exchange(false)) {
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
        return;
    }

    auto stopped = std::make_shared<std::promise<void>>();
    auto stoppedFuture = stopped->get_future();
    asio::post(ioc_, [this, stopped]() {
        timeout_timer_.cancel();
        std::vector<utp_socket*> sockets;
        sockets.reserve(streams_.size());
        for (const auto& [socket, stream] : streams_) {
            sockets.push_back(socket);
            stream->on_closed("uTP engine stopped");
            stream->socket_.store(nullptr);
        }
        for (auto* socket : sockets) {
            utp_close(socket);
        }
        streams_.clear();
        if (context_) {
            utp_destroy(context_);
            context_ = nullptr;
        }
        asio::error_code ec;
        socket_.close(ec);
        stopped->set_value();
    });

    if (stoppedFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        ioc_.stop();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

bool UtpEngine::is_running() const noexcept {
    return running_.load();
}

std::uint16_t UtpEngine::local_port() const noexcept {
    return local_port_;
}

void UtpEngine::set_accept_handler(AcceptHandler handler) {
    accept_handler_ = std::move(handler);
}

void UtpEngine::set_datagram_handler(DatagramHandler handler) {
    datagram_handler_ = std::move(handler);
}

void UtpEngine::allow_peer(const asio::ip::udp::endpoint& endpoint) {
    if (!endpoint.address().is_v4() || endpoint.port() == 0) {
        return;
    }
    const std::string key = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    asio::post(ioc_, [this, key]() {
        if (running_.load()) {
            allowed_peer_endpoints_.insert(key);
        }
    });
}

void UtpEngine::send_datagram(
    const asio::ip::udp::endpoint& endpoint,
    std::string payload) {
    if (!running_.load() || !endpoint.address().is_v4() || payload.empty()) {
        return;
    }
    auto data = std::make_shared<std::string>(std::move(payload));
    asio::post(ioc_, [this, endpoint, data]() {
        if (!running_.load() || !socket_.is_open()) {
            return;
        }
        socket_.async_send_to(asio::buffer(*data), endpoint,
                              [data](const asio::error_code&, std::size_t) {});
    });
}

std::shared_ptr<UtpStream> UtpEngine::connect_to(
    const asio::ip::udp::endpoint& endpoint,
    std::chrono::milliseconds timeout) {
    if (!running_.load() || !endpoint.address().is_v4() || endpoint.port() == 0) {
        throw std::runtime_error("uTP engine is stopped or endpoint is invalid");
    }

    auto created = std::make_shared<std::promise<std::shared_ptr<UtpStream>>>();
    auto createdFuture = created->get_future();
    asio::post(ioc_, [this, endpoint, created]() {
        try {
            if (!context_ || !running_.load()) {
                throw std::runtime_error("uTP engine is stopped");
            }
            utp_socket* socket = utp_create_socket(context_);
            if (!socket) {
                throw std::runtime_error("libutp failed to create a socket");
            }
            auto stream = std::shared_ptr<UtpStream>(
                new UtpStream(shared_from_this(), socket, endpoint));
            utp_set_userdata(socket, stream.get());
            streams_.emplace(socket, stream);
            allowed_peer_endpoints_.insert(
                endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
            const auto address = endpoint.address().to_v4().to_bytes();
            sockaddr_in remote{};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(endpoint.port());
            std::memcpy(&remote.sin_addr, address.data(), address.size());
            if (utp_connect(socket, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0) {
                streams_.erase(socket);
                stream->on_closed("libutp rejected the connection attempt");
                utp_close(socket);
                throw std::runtime_error("libutp rejected the connection attempt");
            }
            created->set_value(std::move(stream));
        } catch (...) {
            created->set_exception(std::current_exception());
        }
    });

    auto stream = createdFuture.get();
    std::unique_lock<std::mutex> lock(stream->read_mutex_);
    const bool connected = stream->read_cv_.wait_for(lock, timeout, [&]() {
        return stream->connected_.load() || !stream->open_.load();
    });
    if (!connected || !stream->connected_.load()) {
        lock.unlock();
        stream->close();
        if (!connected) {
            throw std::runtime_error("uTP connection timed out");
        }
        throw std::runtime_error("uTP connection failed");
    }
    return stream;
}

void UtpEngine::receive_datagram() {
    if (!running_.load() || !socket_.is_open()) {
        return;
    }
    socket_.async_receive_from(
        asio::buffer(receive_buffer_), remote_endpoint_,
        [this](const asio::error_code& ec, std::size_t bytesReceived) {
            if (!ec && context_ && remote_endpoint_.address().is_v4()) {
                bool consumed = false;
                if (datagram_handler_) {
                    consumed = datagram_handler_(
                        receive_buffer_.data(), bytesReceived, remote_endpoint_);
                }
                if (!consumed) {
                    utp_process_udp(
                        context_, reinterpret_cast<const byte*>(receive_buffer_.data()),
                        bytesReceived,
                        reinterpret_cast<const sockaddr*>(remote_endpoint_.data()),
                        static_cast<socklen_t>(remote_endpoint_.size()));
                }
            }
            if (running_.load()) {
                receive_datagram();
            }
        });
}

void UtpEngine::check_timeouts() {
    if (!running_.load()) {
        return;
    }
    timeout_timer_.expires_after(std::chrono::milliseconds(100));
    timeout_timer_.async_wait([this](const asio::error_code& ec) {
        if (!ec && running_.load() && context_) {
            utp_check_timeouts(context_);
            utp_issue_deferred_acks(context_);
            check_timeouts();
        }
    });
}

void UtpEngine::queue_datagram(
    const sockaddr* address,
    socklen_t address_length,
    const byte* data,
    std::size_t size) {
    if (!address || !data || size == 0 || address->sa_family != AF_INET ||
        address_length < sizeof(sockaddr_in) || !socket_.is_open()) {
        return;
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    asio::ip::address_v4::bytes_type addressBytes{};
    std::memcpy(addressBytes.data(), &ipv4->sin_addr, addressBytes.size());
    const asio::ip::udp::endpoint endpoint(
        asio::ip::address_v4(addressBytes), ntohs(ipv4->sin_port));
    auto payload = std::make_shared<std::string>(reinterpret_cast<const char*>(data), size);
    socket_.async_send_to(asio::buffer(*payload), endpoint,
                          [payload](const asio::error_code&, std::size_t) {});
}

void UtpEngine::enqueue_write(
    const std::shared_ptr<UtpStream>& stream,
    const std::shared_ptr<UtpWriteRequest>& request) {
    asio::post(ioc_, [this, stream, request]() {
        if (!running_.load() || !stream->open_.load() || !stream->socket_.load()) {
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                request->error = "uTP connection is closed";
                request->complete = true;
            }
            request->cv.notify_all();
            return;
        }
        stream->write_queue_.push_back(request);
        pump_write(stream);
    });
}

void UtpEngine::pump_write(const std::shared_ptr<UtpStream>& stream) {
    while (!stream->write_queue_.empty()) {
        auto request = stream->write_queue_.front();
        utp_socket* socket = stream->socket_.load();
        if (!stream->open_.load() || !socket) {
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                request->error = "uTP connection is closed";
                request->complete = true;
            }
            request->cv.notify_all();
            stream->write_queue_.pop_front();
            continue;
        }

        const std::size_t remaining = request->bytes.size() - request->offset;
        const auto written = utp_write(
            socket, request->bytes.data() + request->offset, remaining);
        if (written < 0) {
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                request->error = "libutp write failed";
                request->complete = true;
            }
            request->cv.notify_all();
            stream->write_queue_.pop_front();
            continue;
        }
        if (written == 0) {
            return;
        }

        request->offset += static_cast<std::size_t>(written);
        if (request->offset == request->bytes.size()) {
            {
                std::lock_guard<std::mutex> lock(request->mutex);
                request->complete = true;
            }
            request->cv.notify_all();
            stream->write_queue_.pop_front();
        }
    }
}

void UtpEngine::close_socket(utp_socket* socket) noexcept {
    if (!socket) {
        return;
    }
    asio::post(ioc_, [this, socket]() {
        if (context_) {
            if (auto stream = find_stream(socket)) {
                stream->on_closed("uTP connection closed");
                stream->socket_.store(nullptr);
            }
            utp_close(socket);
        }
    });
}

std::shared_ptr<UtpStream> UtpEngine::find_stream(utp_socket* socket) {
    const auto it = streams_.find(socket);
    return it == streams_.end() ? nullptr : it->second;
}

std::shared_ptr<UtpStream> UtpEngine::accept_socket(utp_socket* socket) {
    if (!socket || !accept_handler_) {
        return nullptr;
    }
    sockaddr_in remote{};
    socklen_t remoteLength = sizeof(remote);
    if (utp_getpeername(socket, reinterpret_cast<sockaddr*>(&remote), &remoteLength) != 0) {
        return nullptr;
    }
    asio::ip::address_v4::bytes_type addressBytes{};
    std::memcpy(addressBytes.data(), &remote.sin_addr, addressBytes.size());
    const asio::ip::udp::endpoint endpoint(
        asio::ip::address_v4(addressBytes), ntohs(remote.sin_port));
    auto stream = std::shared_ptr<UtpStream>(
        new UtpStream(shared_from_this(), socket, endpoint));
    utp_set_userdata(socket, stream.get());
    streams_.emplace(socket, stream);
    stream->on_connected();
    accept_handler_(stream);
    return stream;
}

uint64 UtpEngine::on_send_to(utp_callback_arguments* args) {
    auto* engine = static_cast<UtpEngine*>(utp_context_get_userdata(args->context));
    if (engine) {
        engine->queue_datagram(args->address, args->address_len, args->buf, args->len);
    }
    return 0;
}

uint64 UtpEngine::on_accept(utp_callback_arguments* args) {
    auto* engine = static_cast<UtpEngine*>(utp_context_get_userdata(args->context));
    return engine && engine->accept_socket(args->socket) ? 0 : 1;
}

uint64 UtpEngine::on_firewall(utp_callback_arguments* args) {
    auto* engine = static_cast<UtpEngine*>(utp_context_get_userdata(args->context));
    if (!engine || !engine->running_.load() || !args->address ||
        args->address->sa_family != AF_INET || args->address_len < sizeof(sockaddr_in)) {
        return 1;
    }
    const auto* remote = reinterpret_cast<const sockaddr_in*>(args->address);
    asio::ip::address_v4::bytes_type addressBytes{};
    std::memcpy(addressBytes.data(), &remote->sin_addr, addressBytes.size());
    const std::string key = asio::ip::address_v4(addressBytes).to_string() + ":" +
                            std::to_string(ntohs(remote->sin_port));
    return engine->allowed_peer_endpoints_.find(key) != engine->allowed_peer_endpoints_.end() ? 0 : 1;
}

uint64 UtpEngine::on_read(utp_callback_arguments* args) {
    auto* stream = static_cast<UtpStream*>(utp_get_userdata(args->socket));
    if (stream && args->buf && args->len > 0) {
        stream->on_read(args->buf, args->len);
        utp_read_drained(args->socket);
    }
    return 0;
}

uint64 UtpEngine::on_error(utp_callback_arguments* args) {
    auto* stream = static_cast<UtpStream*>(utp_get_userdata(args->socket));
    if (stream) {
        stream->on_closed(utp_error_code_names[args->error_code]);
    }
    return 0;
}

uint64 UtpEngine::on_state_change(utp_callback_arguments* args) {
    auto* engine = static_cast<UtpEngine*>(utp_context_get_userdata(args->context));
    if (!engine) {
        return 0;
    }
    auto stream = engine->find_stream(args->socket);
    if (!stream) {
        return 0;
    }

    if (args->state == UTP_STATE_CONNECT) {
        stream->on_connected();
        engine->pump_write(stream);
    } else if (args->state == UTP_STATE_WRITABLE) {
        engine->pump_write(stream);
    } else if (args->state == UTP_STATE_EOF) {
        stream->on_closed("uTP peer closed the connection");
    } else if (args->state == UTP_STATE_DESTROYING) {
        stream->socket_.store(nullptr);
        stream->on_closed("uTP connection was destroyed");
        engine->streams_.erase(args->socket);
    }
    return 0;
}

uint64 UtpEngine::on_read_buffer_size(utp_callback_arguments*) {
    return 0;
}

UtpStream::UtpStream(
    std::shared_ptr<UtpEngine> engine,
    utp_socket* socket,
    asio::ip::udp::endpoint endpoint)
    : engine_(std::move(engine)), socket_(socket), remote_endpoint_(std::move(endpoint)) {}

UtpStream::~UtpStream() {
    close();
}

void UtpStream::send_all(
    const char* data,
    std::size_t size,
    std::chrono::milliseconds timeout) {
    if (!is_open()) {
        throw std::runtime_error("uTP connection is closed");
    }
    if (size == 0) {
        return;
    }

    std::lock_guard<std::mutex> writeLock(write_mutex_);
    auto request = std::make_shared<UtpWriteRequest>();
    request->bytes.assign(data, size);
    engine_->enqueue_write(shared_from_this(), request);

    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->cv.wait_for(lock, timeout, [&]() { return request->complete; })) {
        lock.unlock();
        close();
        throw std::runtime_error("uTP write timed out");
    }
    if (!request->error.empty()) {
        throw std::runtime_error(request->error);
    }
}

void UtpStream::receive_exact(
    char* data,
    std::size_t size,
    std::chrono::milliseconds timeout) {
    if (size == 0) {
        return;
    }
    std::unique_lock<std::mutex> lock(read_mutex_);
    const auto available = [&]() { return received_.size() - read_offset_; };
    if (!read_cv_.wait_for(lock, timeout, [&]() {
            return available() >= size || !open_.load();
        })) {
        lock.unlock();
        close();
        throw std::runtime_error("uTP read timed out");
    }
    if (available() < size) {
        throw std::runtime_error(read_error_.empty() ? "uTP peer closed the connection" : read_error_);
    }
    std::memcpy(data, received_.data() + read_offset_, size);
    read_offset_ += size;
    if (read_offset_ == received_.size()) {
        received_.clear();
        read_offset_ = 0;
    } else if (read_offset_ > 65536) {
        received_.erase(0, read_offset_);
        read_offset_ = 0;
    }
}

void UtpStream::close() noexcept {
    if (open_.exchange(false)) {
        read_cv_.notify_all();
        if (engine_) {
            engine_->close_socket(socket_.exchange(nullptr));
        }
    }
}

bool UtpStream::is_open() const noexcept {
    return open_.load();
}

bool UtpStream::is_connected() const noexcept {
    return connected_.load();
}

const asio::ip::udp::endpoint& UtpStream::remote_endpoint() const noexcept {
    return remote_endpoint_;
}

void UtpStream::on_connected() {
    connected_.store(true);
    read_cv_.notify_all();
}

void UtpStream::on_read(const byte* data, std::size_t size) {
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        received_.append(reinterpret_cast<const char*>(data), size);
    }
    read_cv_.notify_all();
}

void UtpStream::on_closed(const std::string& error) {
    open_.store(false);
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (!error.empty()) {
            read_error_ = error;
        }
    }
    for (const auto& request : write_queue_) {
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (!request->complete) {
                request->error = error.empty() ? "uTP connection is closed" : error;
                request->complete = true;
            }
        }
        request->cv.notify_all();
    }
    read_cv_.notify_all();
}

}  // namespace pbl4::network