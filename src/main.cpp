#include "webui/api_routes.h"
#include "webui/http_server.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (const std::exception&) {
            std::cerr << "Invalid port argument, using default 8080" << std::endl;
        }
    }

    // Tìm thư mục web/ cạnh project root. exe có thể nằm ở build/ (Ninja/Make)
    // hoặc build/<config>/ (Visual Studio multi-config), nên thử lần lượt
    // 1, 2 cấp lên từ thư mục chứa exe, rồi mới tới thư mục đang chạy lệnh.
    const std::filesystem::path exeDir = std::filesystem::absolute(argv[0]).parent_path();
    std::filesystem::path webRoot;
    for (const std::filesystem::path& candidate : {
             exeDir / "web",
             exeDir / ".." / "web",
             exeDir / ".." / ".." / "web",
             std::filesystem::current_path() / "web",
         }) {
        if (std::filesystem::exists(candidate / "index.html")) {
            webRoot = candidate;
            break;
        }
    }
    if (webRoot.empty()) {
        std::cerr << "Could not locate web/ directory (looked next to the executable and the current directory)" << std::endl;
        return 1;
    }

    const std::filesystem::path tempDir = std::filesystem::current_path() / "data" / ".uploads";

    p2p::HttpServer server(webRoot);
    p2p::ApiRoutes::registerRoutes(server.raw(), tempDir);

    if (!server.run(port)) {
        std::cerr << "Failed to start web server on port " << port << std::endl;
        return 1;
    }

    return 0;
}
