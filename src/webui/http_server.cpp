#include "webui/http_server.h"

#include "httplib.h"

#include <iostream>

namespace p2p {
namespace {

// Tách hostname khỏi "host[:port]" hoặc "scheme://host[:port]"; cho phép gọi qua LAN IP
// khi phát triển/test DHT bootstrap trên cùng mạng nội bộ.
bool isAllowedHost(std::string value) {
    const std::size_t scheme = value.find("://");
    if (scheme != std::string::npos) {
        value.erase(0, scheme + 3);
    }
    const std::size_t slash = value.find('/');
    if (slash != std::string::npos) {
        value.erase(slash);
    }
    const std::size_t colon = value.rfind(':');
    if (colon != std::string::npos) {
        value.erase(colon);
    }
    return value == "127.0.0.1" || value == "localhost" || value == "0.0.0.0" ||
           value.rfind("10.", 0) == 0 || value.rfind("192.168.", 0) == 0 ||
           value.rfind("172.", 0) == 0;
}

}  // namespace

HttpServer::HttpServer(std::filesystem::path staticRoot)
    : staticRoot_(std::move(staticRoot)), server_(new httplib::Server()) {
    // Chỉ phục vụ file tĩnh; API được api_routes.cpp đăng ký thêm vào raw().
    server_->set_mount_point("/", staticRoot_.string());

    // Chấp nhận cả localhost và các IP nội bộ trên cùng LAN để test bootstrap DHT.
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!isAllowedHost(req.get_header_value("Host"))) {
            res.status = 403;
            res.set_content("Forbidden: invalid Host header", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        // Chống CSRF: request đổi trạng thái từ trang web khác luôn mang Origin lạ.
        if (req.method != "GET" && req.method != "HEAD") {
            const std::string origin = req.get_header_value("Origin");
            const bool crossSite = req.get_header_value("Sec-Fetch-Site") == "cross-site";
            if (crossSite || (!origin.empty() && !isAllowedHost(origin))) {
                res.status = 403;
                res.set_content("Forbidden: cross-origin request", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

HttpServer::~HttpServer() {
    delete server_;
}

httplib::Server& HttpServer::raw() {
    return *server_;
}

bool HttpServer::run(int port) {
    // Bind all interfaces so other machines on the same LAN can reach the API endpoints.
    std::cout << "Web UI: http://0.0.0.0:" << port << "/" << std::endl;
    return server_->listen("0.0.0.0", port);
}

}  // namespace p2p
