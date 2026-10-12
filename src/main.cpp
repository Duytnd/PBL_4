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

    // web/ nằm cạnh thư mục chạy (hoặc 2 cấp trên nếu chạy từ build/<config>/).
    const std::filesystem::path exeDir = std::filesystem::absolute(argv[0]).parent_path();
    std::filesystem::path webRoot = exeDir / "web";
    if (!std::filesystem::exists(webRoot)) {
        webRoot = exeDir / ".." / ".." / "web";
    }
    if (!std::filesystem::exists(webRoot)) {
        webRoot = std::filesystem::current_path() / "web";
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
