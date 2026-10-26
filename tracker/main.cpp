#include "httplib.h"

#include "tracker/tracker_server.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

// Đọc 1 tham số dạng số từ query, trả về giá trị mặc định nếu thiếu / sai.
std::uint64_t paramU64(const httplib::Request& req, const char* key, std::uint64_t fallback = 0) {
    if (!req.has_param(key)) {
        return fallback;
    }
    try {
        return std::stoull(req.get_param_value(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

int main(int argc, char** argv) {
    int port = 6969;
    int interval = 1800;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (const std::exception&) {
            std::cerr << "Invalid port argument, using default 6969" << std::endl;
        }
    }

    p2p::TrackerServer tracker(interval, /*maxPeers=*/50);

    httplib::Server server;

    // Dọn peer hết hạn định kỳ, không chờ tới khi có request mới.
    std::thread cleanupThread([&tracker]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            tracker.registry().purgeExpired();
        }
    });
    cleanupThread.detach();

    server.Get("/announce", [&tracker](const httplib::Request& req, httplib::Response& res) {
        // info_hash / peer_id là byte nhị phân; httplib tự percent-decode
        // query string nên get_param_value trả đúng 20 byte gốc (mục 5.2).
        p2p::TrackerRequest request;
        request.infoHash = req.get_param_value("info_hash");
        request.peerId = req.get_param_value("peer_id");
        const std::uint64_t rawPort = paramU64(req, "port");
        if (rawPort == 0 || rawPort > 65535) {
            res.set_content(p2p::TrackerProtocol::encodeFailureResponse("invalid port"), "text/plain");
            return;
        }
        request.port = static_cast<std::uint16_t>(rawPort);
        request.uploaded = paramU64(req, "uploaded");
        request.downloaded = paramU64(req, "downloaded");
        request.left = paramU64(req, "left");
        request.event = req.get_param_value("event");
        request.numwant = req.has_param("numwant") ? static_cast<int>(paramU64(req, "numwant", 50)) : 50;
        request.compact = true;
        // Không dùng "ip" người gửi tự khai: lấy IP thật của kết nối TCP để
        // tránh client giả mạo địa chỉ peer khác (tránh bị lợi dụng làm DDoS).
        request.ip = req.remote_addr;

        // event do client tự khai: chỉ log giá trị hợp lệ để không chèn dòng giả vào log.
        const bool knownEvent = request.event == "started" || request.event == "completed" ||
                                request.event == "stopped";
        std::cout << "[TRACKER] Announce received from " << request.ip << ":" << request.port
                  << " | event=" << (knownEvent ? request.event : "interval")
                  << " | left=" << request.left
                  << " | up=" << request.uploaded << " | down=" << request.downloaded << std::endl;

        const std::string response = tracker.announce(request);
        res.set_content(response, "text/plain");
    });

    std::cout << "Tracker server (BEP 3 + BEP 23): http://0.0.0.0:" << port << "/announce" << std::endl;
    std::cout << "Announce interval: " << interval << "s, peer expiry: " << (interval * 2) << "s" << std::endl;

    // Khác với web UI: tracker phải cho các máy khác trong LAN announce tới,
    // nên bind 0.0.0.0, không chỉ 127.0.0.1.
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start tracker server on port " << port << std::endl;
        return 1;
    }

    return 0;
}
