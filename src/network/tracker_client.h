#pragma once

#include "network/tracker_protocol.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace p2p {

// Kết quả kiểm tra một URL tracker trước khi liên lạc.
struct TrackerUrlCheck {
    bool allowed = false;
    std::string reason;  // lý do từ chối (rỗng khi allowed)
};

class TrackerClient {
public:
    // Tracker trong magnet link / file .torrent là dữ liệu của người lạ. Nếu gọi mù, kẻ tạo magnet bắt client gửi
    // HTTP tới bất kỳ dịch vụ nội bộ nào (WebUI của chính mình, router, cloud metadata...) với query do họ chọn (SSRF).
    // Kiểm tra bắt buộc cho mọi yêu cầu ra tracker (announceHttp/announceUdp đều gọi):
    //  - scheme chỉ http://, https:// hoặc udp://, host không rỗng;
    //  - host không phải 0.0.0.0, link-local 169.254.x.x (cloud metadata), multicast/broadcast, hay tên metadata cloud;
    //  - cổng không thuộc nhóm dịch vụ nhạy cảm (SSH, SMTP, SMB, RDP, DB, Redis, Memcached...);
    //  - với http(s)://: thành phần cuối của đường dẫn phải bắt đầu bằng "announce" (quy ước BEP 3: /announce,
    //    /announce.php, /abc/announce). Quy tắc này chặn gọi tới /api/..., /cgi-bin/... của dịch vụ khác.
    // Loopback và IP LAN vẫn được phép: tracker trong mạng nội bộ là cách dùng chính của dự án.
    static TrackerUrlCheck checkTrackerUrl(const std::string& url);

    static std::string buildAnnounceRequest(const TrackerRequest& request);
    static std::string makePeerId(const std::string& prefix = "-PB0001-");
    static TrackerResponse announce(const std::string& responseBody, const TrackerRequest& request);
    static TrackerResponse parseAnnounceResponse(const std::string& responseBody);

    // Gửi GET announce thật tới tracker qua HTTP (BEP 3) và parse response.
    // announceUrl dạng "http://host:port/path", chỉ hỗ trợ HTTP thường (không HTTPS;
    // UDP tracker dùng announceUdp).
    // Ném std::runtime_error nếu không kết nối được hoặc HTTP status != 200.
    static TrackerResponse announceHttp(const std::string& announceUrl, const TrackerRequest& request);
    // UDP tracker (BEP 15): connect -> announce qua UDP, retry khi timeout.
    // announceUrl dạng "udp://host:port[/path]". Ném std::runtime_error nếu tracker không
    // phản hồi, std::invalid_argument nếu URL sai. Tracker trả action=error -> success=false.
    static TrackerResponse announceUdp(const std::string& announceUrl, const TrackerRequest& request);

    // Chọn giao thức theo scheme: udp:// -> announceUdp, còn lại -> announceHttp
    // (announceHttp từ chối scheme lạ bằng std::invalid_argument).
    static TrackerResponse announceTracker(const std::string& announceUrl, const TrackerRequest& request);

    // Codec BEP 15 (tách riêng để test không cần mạng). Số nguyên big-endian.
    static std::string buildUdpConnectRequest(std::uint32_t transactionId);
    // Trả true và gán connectionId nếu gói hợp lệ (action=0, đúng transactionId, đủ 16 byte).
    // Nếu tracker trả action=3 (error) thì errorMessage được điền và trả false.
    static bool parseUdpConnectResponse(const std::string& packet, std::uint32_t transactionId,
                                        std::uint64_t& connectionId, std::string& errorMessage);
    static std::string buildUdpAnnounceRequest(std::uint64_t connectionId, std::uint32_t transactionId,
                                               const TrackerRequest& request, std::uint32_t key);
    // success=false + failureReason nếu action=error / sai transactionId / gói quá ngắn.
    static TrackerResponse parseUdpAnnounceResponse(const std::string& packet, std::uint32_t transactionId);

    static std::pair<std::string, std::uint16_t> udpRendezvousEndpoint(
        const std::string& announceUrl);
};

}  // namespace p2p
