#include "webui/http_server.h"

#include "httplib.h"

#include <iostream>

namespace p2p {

HttpServer::HttpServer(std::filesystem::path staticRoot)
    : staticRoot_(std::move(staticRoot)), server_(new httplib::Server()) {
    // Chỉ phục vụ file tĩnh; API được api_routes.cpp đăng ký thêm vào raw().
    server_->set_mount_point("/", staticRoot_.string());

    // Chặn request có header Host khác 127.0.0.1 / localhost: tránh một trang
    // web khác (nguồn khác) điều khiển server qua DNS rebinding (mục 5.7).
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const std::string host = req.get_header_value("Host");
        const bool allowed =
            host.rfind("127.0.0.1", 0) == 0 || host.rfind("localhost", 0) == 0;
        if (!allowed) {
            res.status = 403;
            res.set_content("Forbidden: invalid Host header", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
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
    // Chỉ bind 127.0.0.1 (mục 5.7 của kế hoạch): không cho máy khác trong LAN gọi tới.
    std::cout << "Web UI: http://127.0.0.1:" << port << "/" << std::endl;
    return server_->listen("127.0.0.1", port);
}

}  // namespace p2p
