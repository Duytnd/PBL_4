#include "webui/http_server.h"

#include "httplib.h"

#include "webui/request_guard.h"

#include <iostream>

namespace p2p {

HttpServer::HttpServer(std::filesystem::path staticRoot)
    : staticRoot_(std::move(staticRoot)), server_(new httplib::Server()) {
    // Chỉ phục vụ file tĩnh; API được api_routes.cpp đăng ký thêm vào raw().
    server_->set_mount_point("/", staticRoot_.string());
    // Buộc trình duyệt hỏi lại server (ETag/Last-Modified) cho mỗi file giao diện, nếu không HTML mới có thể ghép
    // với CSS/JS cũ trong cache và vỡ bố cục sau khi cập nhật.
    server_->set_file_request_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
    });

    // Chấp nhận localhost và IP nội bộ (loopback, RFC1918, 100.64/10) dạng IP literal. Không chấp nhận tên miền:
    // trước đây so tiền tố chuỗi nên "10.evil.com" qua được kiểm tra (DNS rebinding).
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const std::string host = req.get_header_value("Host");
        if (!webui::isAllowedHost(host)) {
            res.status = 403;
            res.set_content("Forbidden: invalid Host header", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        // Chống CSRF: request đổi trạng thái từ trang web khác luôn mang Origin lạ.
        if (req.method != "GET" && req.method != "HEAD") {
            const std::string origin = req.get_header_value("Origin");
            const bool crossSite = req.get_header_value("Sec-Fetch-Site") == "cross-site";
            // Origin (nếu có) phải đúng là chính WebUI này, không chỉ "một IP nội bộ nào đó" như trang web
            // của máy khác trong LAN.
            if (crossSite || !webui::isSameOrigin(origin, host)) {
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
