#pragma once

#include "network/tracker_protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace p2p {

struct PeerRegistration {
    std::string peerId;
    std::string ip;
    std::uint16_t port = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t downloaded = 0;
    std::uint64_t left = 0;
    std::string key;  // khoá phiên 8 byte thô; rỗng hoặc toàn byte 0 = không có (client cũ)
};

// Khoá "không có": rỗng hoặc toàn byte 0.
inline bool isNoKey(const std::string& key) {
    return key.empty() || key.find_first_not_of('\0') == std::string::npos;
}

// Mục có khoá thì chỉ người trình đúng khoá mới thao tác được. Mục không có khoá (client cũ) ai cũng thao tác được.
inline bool keyAuthorizes(const std::string& entryKey, const std::string& givenKey) {
    return isNoKey(entryKey) || entryKey == givenKey;
}

// Lưu danh sách peer theo info_hash, tự xoá peer đã hết hạn.
// Tách khỏi TrackerServer để test được riêng và vì đây là phần dữ liệu
// thuần (mục 6 của kế hoạch: tracker/peer_registry).
class PeerRegistry {
public:
    using Clock = std::chrono::steady_clock;

    // expirySeconds: peer không announce lại trong khoảng này sẽ bị coi là
    // đã rời mạng (mục 5.2: "thời gian hết hạn, vd 2 x interval").
    //
    // maxPeersPerIpPerSwarm: số peer tối đa MỖI IP trong 1 swarm. Vượt thì peer cũ nhất CỦA CHÍNH IP ĐÓ bị
    // thay, không đụng tới peer của IP khác. Một IP thay peer_id liên tục (peer_id do client tự chọn) không
    // thể đẩy peer thật ra khỏi swarm giới hạn 50 chỗ nữa. Mặc định 10 (nhiều client chung 1 IP/NAT vẫn đủ).
    // maxSwarmsPerIp: số swarm tối đa 1 IP được tham gia, chỉ kiểm khi bảng swarm đã đầy nửa
    // (chống 1 IP tạo hàng nghìn info_hash giả để lấp đầy bảng).
    explicit PeerRegistry(int expirySeconds, std::size_t maxPeersPerTorrent,
                          std::size_t maxSwarms = 10000,
                          std::size_t maxPeersPerIpPerSwarm = 10,
                          std::size_t maxSwarmsPerIp = 100);

    // now cho phép test giả lập thời gian trôi qua mà không cần sleep thật.
    // Trả false khi swarm mới bị từ chối vì registry đã đầy (chống phình RAM).
    enum class UpsertStatus { Ok, SwarmFull, KeyMismatch };
    UpsertStatus upsertChecked(const std::string& infoHash, const PeerRegistration& peer,
                               Clock::time_point now = Clock::now());
    bool upsert(const std::string& infoHash, const PeerRegistration& peer,
                Clock::time_point now = Clock::now());
    // key: nếu mục của peer_id có khoá thì phải khớp mới xoá được (chống người khác gửi "stopped" thay peer).
    void remove(const std::string& infoHash, const std::string& peerId, const std::string& key = {});

    // Dọn mọi peer đã hết hạn trên toàn bộ registry.
    void purgeExpired(Clock::time_point now = Clock::now());

    [[nodiscard]] std::vector<TrackerPeer> getPeers(
        const std::string& infoHash,
        const std::string& excludePeerId = "",
        Clock::time_point now = Clock::now()) const;

    [[nodiscard]] bool hasPeer(const std::string& infoHash, const std::string& peerId) const;
    [[nodiscard]] std::size_t peerCount(const std::string& infoHash) const;
    bool updateUdpEndpoint(
        const std::string& infoHash,
        const std::string& peerId,
        const std::string& sourceIp,
        std::uint16_t sourcePort,
        std::uint16_t announcedTcpPort,
        Clock::time_point now = Clock::now(),
        const std::string& key = {});
    [[nodiscard]] std::optional<TrackerPeer> getUdpEndpointByTcpEndpoint(
        const std::string& infoHash,
        const std::string& requesterPeerId,
        const std::string& requesterIp,
        const std::string& targetIp,
        std::uint16_t targetTcpPort,
        Clock::time_point now = Clock::now(),
        const std::string& requesterKey = {}) const;

private:
    struct Entry {
        PeerRegistration peer;
        Clock::time_point lastSeen;
        std::string udpIp;
        std::uint16_t udpPort = 0;
        Clock::time_point udpLastSeen{};
    };

    int expirySeconds_;
    std::size_t maxPeersPerTorrent_;
    std::size_t maxSwarms_;
    std::size_t maxPeersPerIpPerSwarm_;
    std::size_t maxSwarmsPerIp_;
    // httplib xử lý announce đa luồng + luồng dọn định kỳ: mọi truy cập swarms_ phải khoá.
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Entry>> swarms_;
};

}  // namespace p2p
