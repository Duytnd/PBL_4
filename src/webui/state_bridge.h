#pragma once

#include "network/dht/kademlia_node.h"
#include "network/torrent_session.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace p2p {

struct TorrentViewInfo {
    std::string name;
    std::string info_hash_hex;
    std::string magnet_uri;
    std::string state_name;
    std::uint64_t total_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t uploaded_bytes = 0;
    double progress_percent = 0.0;
    double download_rate = 0.0;    // bytes/s
    double upload_rate = 0.0;      // bytes/s
    std::size_t completed_pieces = 0;
    std::size_t total_pieces = 0;
    std::size_t connected_peers = 0;
    std::size_t dht_nodes = 0;
    std::string error_message;
    bool paused = false;
    bool queued = false;  // đang chờ lượt trong hàng đợi (bị tạm dừng bởi giới hạn số torrent tải cùng lúc)
    std::uint64_t download_limit = 0;  // byte/s, 0 = không giới hạn
    std::uint64_t upload_limit = 0;
    std::string picker_strategy = "rarest_first";  // chiến lược chọn piece hiện tại
};

// Các công tắc tìm peer của một torrent lúc được thêm vào; lưu vào .meta để khôi phục đúng lựa chọn của người dùng.
struct SessionFlags {
    bool enable_tracker = true;
    bool enable_dht = true;
    bool enable_lsd = true;
};

// Torrent đã lưu trên đĩa, dùng để dựng lại session khi mở lại ứng dụng.
struct SavedTorrent {
    TorrentFileMetadata metadata;
    std::string magnet_uri;
    bool paused = false;
    std::uint64_t download_limit = 0;
    std::uint64_t upload_limit = 0;
    bool enable_tracker = true;  // thiếu trong file .meta cũ thì mặc định true
    bool enable_dht = true;
    bool enable_lsd = true;
    std::vector<std::size_t> skipped_files;  // chỉ số các file người dùng đã bỏ chọn
};

class StateBridge {
public:
    static StateBridge& instance() {
        static StateBridge s_instance;
        return s_instance;
    }

    void addSession(std::shared_ptr<TorrentSession> session, const std::string& magnetUri,
                    const SessionFlags& flags = {});
    bool removeSession(const std::string& infoHashHex);
    std::shared_ptr<TorrentSession> getSession(const std::string& infoHashHex);

    // Lưu danh sách torrent (file .torrent dựng lại từ metadata + file .meta chứa magnet/pause/giới hạn)
    // vào thư mục này để mở lại app không mất. Chưa gọi setStateDir thì không lưu gì (dùng cho test).
    void setStateDir(const std::filesystem::path& dir);
    // Ghi lại trạng thái (pause, giới hạn tốc độ) của 1 torrent đang có sau khi người dùng đổi.
    void persistState(const std::string& infoHashHex);
    // Đọc các torrent đã lưu; file hỏng bị bỏ qua.
    [[nodiscard]] std::vector<SavedTorrent> loadSavedTorrents() const;

    // ---- Giới hạn băng thông TỔNG cho cả ứng dụng (byte/s, 0 = không giới hạn) ----
    // Cộng dồn với giới hạn riêng của từng torrent: mỗi block phải đủ token ở cả hai. Lưu cùng thư mục trạng thái.
    void setGlobalLimits(std::uint64_t downBytesPerSec, std::uint64_t upBytesPerSec);
    [[nodiscard]] std::uint64_t globalDownloadLimit() const;
    [[nodiscard]] std::uint64_t globalUploadLimit() const;
    // Gắn hai giới hạn tổng vào cấu hình session sắp tạo.
    void applyGlobalLimiters(TorrentSessionConfig& config) const;

    // ---- Hàng đợi tải ----
    // Chỉ tối đa `max` torrent ĐANG TẢI (chưa xong, không tạm dừng) chạy cùng lúc; phần còn lại bị tạm dừng và xếp
    // hàng theo thứ tự thêm vào, tự chạy khi có chỗ trống. 0 = không giới hạn. Torrent đã xong (seed) không tính.
    // Giá trị được lưu cùng thư mục trạng thái.
    void setMaxActiveDownloads(std::size_t max);
    [[nodiscard]] std::size_t maxActiveDownloads() const;
    // Người dùng tạm dừng / tiếp tục: khác với tạm dừng do hàng đợi. Tiếp tục một torrent đang xếp hàng sẽ đẩy nó
    // lên đầu hàng (chiếm chỗ của torrent mới nhất đang chạy). Trả false nếu không có torrent này.
    bool userPause(const std::string& infoHashHex);
    bool userResume(const std::string& infoHashHex);
    // Áp dụng lại quy tắc hàng đợi ngay (cũng được luồng nền gọi định kỳ để bắt các torrent vừa tải xong).
    void rebalanceQueue();
    [[nodiscard]] bool isQueued(const std::string& infoHashHex) const;

    std::vector<TorrentViewInfo> getAllTorrents();
    std::string toJsonString();

    // ---- Chiến lược chọn piece (piece picker strategy) ----
    // Đặt chiến lược cho torrent theo info_hash. Trả false nếu không tìm thấy session.
    // strategy: "rarest_first" hoặc "sequential"
    bool setPickerStrategy(const std::string& infoHashHex, const std::string& strategy);
    // Lấy chiến lược hiện tại của torrent. Trả chuỗi rỗng nếu không tìm thấy.
    std::string getPickerStrategy(const std::string& infoHashHex) const;
    // Chiến lược MẶC ĐỊNH cho torrent thêm sau này (tạo, thêm, khôi phục sau khi mở lại app). Lưu cùng thư mục trạng thái.
    [[nodiscard]] std::string defaultPickerStrategy() const;
    // Đặt chiến lược cho TẤT CẢ torrent đang có và làm mặc định cho torrent mới. Trả số torrent đã đổi,
    // hoặc -1 nếu tên chiến lược không hợp lệ.
    int setAllPickerStrategies(const std::string& strategy);

    // Cấp phát cổng kế tiếp cho session mới
    std::uint16_t allocatePort();

    // Node DHT dùng chung cho toàn bộ server
    std::shared_ptr<KademliaNode> getOrCreateSharedDht(
        std::uint16_t preferredPort = 6881,
        const std::string& bootstrapHost = "",
        std::uint16_t bootstrapPort = 0);
    std::shared_ptr<KademliaNode> sharedDht() const;

private:
    StateBridge() = default;
    ~StateBridge();

    StateBridge(const StateBridge&) = delete;
    StateBridge& operator=(const StateBridge&) = delete;

    mutable std::mutex mutex_;
    struct Entry {
        std::shared_ptr<TorrentSession> session;
        std::string magnet_uri;
        bool queued = false;       // bị hàng đợi tạm dừng (không phải người dùng)
        std::int64_t order = 0;    // nhỏ hơn = ưu tiên hơn
        std::string picker_strategy = "rarest_first";  // chiến lược chọn piece: "rarest_first" hoặc "sequential"
        SessionFlags flags;
    };
    std::unordered_map<std::string, Entry> sessions_;
    std::shared_ptr<bench::BandwidthLimiter> global_download_ = std::make_shared<bench::BandwidthLimiter>(0);
    std::shared_ptr<bench::BandwidthLimiter> global_upload_ = std::make_shared<bench::BandwidthLimiter>(0);
    std::size_t max_active_ = 0;
    std::string default_picker_strategy_ = "rarest_first";
    std::int64_t next_order_ = 1;
    std::int64_t front_order_ = 0;  // giá trị order nhỏ nhất đã cấp (để đẩy lên đầu hàng)
    std::mutex rebalance_mutex_;    // tuần tự hoá rebalanceQueue (thực thi pause/resume ngoài mutex_)
    std::mutex queue_thread_mutex_;
    std::condition_variable queue_cv_;
    std::thread queue_thread_;
    bool queue_stop_ = false;
    void ensureQueueThread();
    void saveQueueConfigLocked() const;
    std::filesystem::path state_dir_;
    void saveEntryLocked(const std::string& hash, const Entry& entry) const;
    std::uint16_t next_port_ = 6881;
    std::shared_ptr<KademliaNode> shared_dht_;
};

}  // namespace p2p

