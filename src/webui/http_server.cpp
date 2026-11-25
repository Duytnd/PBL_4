#include "webui/http_server.h"

#include "httplib.h"

#include "webui/request_guard.h"

#include <cstdlib>
#include <iostream>

namespace {

// Mặc định WebUI chỉ nhận kết nối từ chính máy này (không có xác thực). Mở ra mạng chỉ khi người dùng chủ động
// đặt P2P_WEBUI_ALLOW_REMOTE=1.
bool allowRemoteAccess() {
    const char* value = std::getenv("P2P_WEBUI_ALLOW_REMOTE");
    return value != nullptr && std::string(value) == "1";
}

}  // namespace

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

    // Header an toàn cho MỌI phản hồi (file tĩnh, API, lỗi).
    server_->set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "no-referrer");
    });

    // Chấp nhận localhost và IP nội bộ (loopback, RFC1918, 100.64/10) dạng IP literal. Không chấp nhận tên miền:
    // trước đây so tiền tố chuỗi nên "10.evil.com" qua được kiểm tra (DNS rebinding).
    server_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        // Phòng thủ chiều sâu: dù đã bind 127.0.0.1, vẫn từ chối peer không phải loopback khi chưa bật remote.
        if (!allowRemoteAccess() && !webui::isLoopbackAddress(req.remote_addr)) {
            res.status = 403;
            res.set_content("Forbidden: Web UI only accepts local connections", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
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
    // Mặc định chỉ bind loopback; mở LAN là tùy chọn qua P2P_WEBUI_ALLOW_REMOTE=1.
    const bool remote = allowRemoteAccess();
    const std::string bindAddress = remote ? "0.0.0.0" : "127.0.0.1";
    if (remote) {
        std::cerr << "WARNING: Web UI is reachable from the network without authentication "
                     "(P2P_WEBUI_ALLOW_REMOTE=1)" << std::endl;
    }
    std::cout << "Web UI: http://" << bindAddress << ":" << port << "/" << std::endl;
    return server_->listen(bindAddress, port);
}

}  // namespace p2p
