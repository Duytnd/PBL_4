#pragma once

#include <filesystem>

namespace httplib {
class Server;
}

namespace p2p {

// Đăng ký các route /api/* vào server. Giai đoạn 2 của plan.
// Hiện có: POST /api/torrents/create (nối trực tiếp tới core::TorrentBuilder).
class ApiRoutes {
public:
    // tempDir: nơi lưu tạm file người dùng tải lên trước khi đưa vào TorrentBuilder.
    static void registerRoutes(httplib::Server& server, const std::filesystem::path& tempDir);
};

}  // namespace p2p
