#pragma once

#include <filesystem>
#include <string>

namespace httplib {
class Server;
}

namespace p2p {

// Bọc httplib::Server: chỉ bind 127.0.0.1, phục vụ file tĩnh trong web/
// và cho đăng ký thêm route API (xem api_routes.h).
class HttpServer {
public:
    explicit HttpServer(std::filesystem::path staticRoot);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Chạy server (blocking) tại 127.0.0.1:port.
    bool run(int port);

    httplib::Server& raw();

private:
    std::filesystem::path staticRoot_;
    httplib::Server* server_;
};

}  // namespace p2p
