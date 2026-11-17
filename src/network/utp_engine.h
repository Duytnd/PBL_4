#pragma once

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "utp.h"

namespace pbl4::network {

class UtpStream;
struct UtpWriteRequest;

class UtpEngine : public std::enable_shared_from_this<UtpEngine> {
public:
    using AcceptHandler = std::function<void(std::shared_ptr<UtpStream>)>;
    using DatagramHandler = std::function<bool(
        const char*, std::size_t, const asio::ip::udp::endpoint&)>;

    UtpEngine();
    ~UtpEngine();

    UtpEngine(const UtpEngine&) = delete;
    UtpEngine& operator=(const UtpEngine&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;

    void set_accept_handler(AcceptHandler handler);
    void set_datagram_handler(DatagramHandler handler);
    void allow_peer(const asio::ip::udp::endpoint& endpoint);
    void send_datagram(const asio::ip::udp::endpoint& endpoint, std::string payload);

    std::shared_ptr<UtpStream> connect_to(
        const asio::ip::udp::endpoint& endpoint,
        std::chrono::milliseconds timeout);

private:
    friend class UtpStream;

    asio::io_context ioc_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timeout_timer_;
    asio::ip::udp::endpoint remote_endpoint_;
    std::array<char, 65536> receive_buffer_{};
    utp_context* context_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::uint16_t local_port_ = 0;
    AcceptHandler accept_handler_;
    DatagramHandler datagram_handler_;
    std::unordered_map<utp_socket*, std::shared_ptr<UtpStream>> streams_;
    std::unordered_set<std::string> allowed_peer_endpoints_;

    void receive_datagram();
    void check_timeouts();
    void queue_datagram(const sockaddr* address, socklen_t address_length,
                        const byte* data, std::size_t size);
    void connect_on_loop(const std::shared_ptr<UtpStream>& stream,
                         const asio::ip::udp::endpoint& endpoint);
    void enqueue_write(const std::shared_ptr<UtpStream>& stream,
                       const std::shared_ptr<UtpWriteRequest>& request);
    void pump_write(const std::shared_ptr<UtpStream>& stream);
    void close_socket(utp_socket* socket) noexcept;
    std::shared_ptr<UtpStream> find_stream(utp_socket* socket);
    std::shared_ptr<UtpStream> accept_socket(utp_socket* socket);

    static uint64 on_send_to(utp_callback_arguments* args);
    static uint64 on_accept(utp_callback_arguments* args);
    static uint64 on_firewall(utp_callback_arguments* args);
    static uint64 on_read(utp_callback_arguments* args);
    static uint64 on_error(utp_callback_arguments* args);
    static uint64 on_state_change(utp_callback_arguments* args);
    static uint64 on_read_buffer_size(utp_callback_arguments* args);
};

struct UtpWriteRequest {
    std::string bytes;
    std::size_t offset = 0;
    bool complete = false;
    std::string error;
    std::mutex mutex;
    std::condition_variable cv;
};

class UtpStream : public std::enable_shared_from_this<UtpStream> {
public:
    ~UtpStream();

    UtpStream(const UtpStream&) = delete;
    UtpStream& operator=(const UtpStream&) = delete;

    void send_all(const char* data, std::size_t size, std::chrono::milliseconds timeout);
    void receive_exact(char* data, std::size_t size, std::chrono::milliseconds timeout);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] const asio::ip::udp::endpoint& remote_endpoint() const noexcept;

private:
    friend class UtpEngine;

    UtpStream(std::shared_ptr<UtpEngine> engine, utp_socket* socket,
              asio::ip::udp::endpoint endpoint);

    std::shared_ptr<UtpEngine> engine_;
    std::atomic<utp_socket*> socket_{nullptr};
    asio::ip::udp::endpoint remote_endpoint_;
    std::atomic<bool> open_{true};
    std::atomic<bool> connected_{false};
    std::mutex read_mutex_;
    std::condition_variable read_cv_;
    std::string received_;
    std::size_t read_offset_ = 0;
    std::string read_error_;
    std::mutex write_mutex_;
    std::deque<std::shared_ptr<UtpWriteRequest>> write_queue_;

    void on_connected();
    void on_read(const byte* data, std::size_t size);
    void on_closed(const std::string& error = {});
};

}  // namespace pbl4::network