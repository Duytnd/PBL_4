#include "httplib.h"

#include "asio/asio.hpp"
#include "network/udp_rendezvous.h"
#include "tracker/tracker_server.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

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

    asio::io_context udpIoc;
    asio::ip::udp::socket udpSocket(udpIoc);
    std::array<char, 128> udpBuffer{};
    asio::ip::udp::endpoint udpRemote;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastPunchByEndpoint;
    std::function<void()> receiveUdp;
    std::thread udpThread;
    bool udpEnabled = false;

    if (port > 0 && port <= 65535) {
        try {
            udpSocket.open(asio::ip::udp::v4());
            udpSocket.bind(asio::ip::udp::endpoint(
                asio::ip::udp::v4(), static_cast<std::uint16_t>(port)));
            udpEnabled = true;

            receiveUdp = [&]() {
                udpSocket.async_receive_from(
                    asio::buffer(udpBuffer), udpRemote,
                    [&](const asio::error_code& ec, std::size_t bytesReceived) {
                        if (!ec) {
                            p2p::network::udp_rendezvous::Request request;
                            const bool valid = p2p::network::udp_rendezvous::decode_request(
                                udpBuffer.data(), bytesReceived, request);
                            if (valid && udpRemote.address().is_v4()) {
                                const std::string sourceIp = udpRemote.address().to_v4().to_string();
                                const std::uint16_t sourcePort = udpRemote.port();
                                std::string reply;

                                if (request.opcode == p2p::network::udp_rendezvous::Opcode::Register) {
                                    const bool registered = tracker.registerUdpEndpoint(
                                        request.info_hash, request.peer_id, sourceIp, sourcePort,
                                        request.peer_tcp_port);
                                    reply = p2p::network::udp_rendezvous::encode_register_result(
                                        registered ? p2p::network::udp_rendezvous::Status::Ok
                                                   : p2p::network::udp_rendezvous::Status::Invalid);
                                } else {
                                    const std::string rateKey = sourceIp + ":" + std::to_string(sourcePort);
                                    const auto now = std::chrono::steady_clock::now();
                                    const auto previous = lastPunchByEndpoint.find(rateKey);
                                    if (previous != lastPunchByEndpoint.end() &&
                                        now - previous->second < std::chrono::milliseconds(25)) {
                                        receiveUdp();
                                        return;
                                    }
                                    lastPunchByEndpoint[rateKey] = now;

                                    const bool registered = tracker.registerUdpEndpoint(
                                        request.info_hash, request.peer_id, sourceIp, sourcePort,
                                        request.peer_tcp_port);
                                    const auto target = registered
                                        ? tracker.rendezvousEndpoint(
                                              request.info_hash, request.peer_id, sourceIp,
                                              request.target_ip, request.target_tcp_port)
                                        : std::nullopt;
                                    if (target) {
                                        reply = p2p::network::udp_rendezvous::encode_punch_result(
                                            p2p::network::udp_rendezvous::Status::Ok,
                                            request.info_hash, request.transaction_id,
                                            target->ip, target->port);
                                        const auto notification = p2p::network::udp_rendezvous::encode_notification(
                                            request.info_hash, sourceIp, request.peer_tcp_port,
                                            sourceIp, sourcePort);
                                        const auto targetAddress = asio::ip::make_address_v4(target->ip);
                                        auto notificationPayload = std::make_shared<std::string>(notification);
                                        udpSocket.async_send_to(
                                            asio::buffer(*notificationPayload),
                                            asio::ip::udp::endpoint(targetAddress, target->port),
                                            [notificationPayload](const asio::error_code&, std::size_t) {});
                                    } else {
                                        reply = p2p::network::udp_rendezvous::encode_punch_result(
                                            registered
                                                ? p2p::network::udp_rendezvous::Status::EndpointNotReady
                                                : p2p::network::udp_rendezvous::Status::Invalid,
                                            request.info_hash, request.transaction_id);
                                    }
                                }

                                auto replyPayload = std::make_shared<std::string>(std::move(reply));
                                udpSocket.async_send_to(
                                    asio::buffer(*replyPayload), udpRemote,
                                    [replyPayload](const asio::error_code&, std::size_t) {});
                            }
                        }
                        if (udpSocket.is_open()) {
                            receiveUdp();
                        }
                    });
            };
            receiveUdp();
            udpThread = std::thread([&udpIoc]() { udpIoc.run(); });
        } catch (const std::exception& ex) {
            std::cerr << "UDP rendezvous unavailable on port " << port << ": " << ex.what() << std::endl;
            asio::error_code ec;
            udpSocket.close(ec);
        }
    }

    std::cout << "Tracker server (BEP 3 + BEP 23): http://0.0.0.0:" << port << "/announce" << std::endl;
    if (udpEnabled) {
        std::cout << "UDP rendezvous: 0.0.0.0:" << port << "/udp" << std::endl;
    }
    std::cout << "Announce interval: " << interval << "s, peer expiry: " << (interval * 2) << "s" << std::endl;

    // Khác với web UI: tracker phải cho các máy khác trong LAN announce tới,
    // nên bind 0.0.0.0, không chỉ 127.0.0.1.
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start tracker server on port " << port << std::endl;
        if (udpEnabled) {
            asio::post(udpIoc, [&udpSocket]() {
                asio::error_code ec;
                udpSocket.close(ec);
            });
            udpThread.join();
        }
        return 1;
    }

    if (udpEnabled) {
        asio::post(udpIoc, [&udpSocket]() {
            asio::error_code ec;
            udpSocket.close(ec);
        });
        udpThread.join();
    }

    return 0;
}
