#pragma once

#include "core/piece_manager.h"
#include "core/torrent_file.h"
#include "network/peer_connection.h"
#include "network/peer_protocol.h"
#include "network/tracker_client.h"
#include "network/tracker_protocol.h"
#include "strategy/choker.h"
#include "strategy/piece_picker.h"
#include "network/dht/kademlia_node.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p2p {
class LocalServiceDiscovery;
struct LsdPeer;
}

namespace p2p::network {

// Trạng thái hoạt động của TorrentSession
enum class SessionState {
    Stopped,      // Chưa chạy hoặc đã dừng
    Checking,     // Đang quét file có sẵn trên đĩa để xác minh hash
    Downloading,  // Đang kết nối peer và tải block
    Waiting,      // Đang chờ peer/tracker phản hồi; không phải lỗi nghiêm trọng
    Seeding,      // Đã đủ 100% dữ liệu, đang làm Seeder chia sẻ cho peer khác
    Paused,       // Tạm dừng tải/seed
    Error         // Gặp lỗi nghiêm trọng (I/O, tracker, v.v.)
};

inline std::ostream& operator<<(std::ostream& os, SessionState s) {
    switch (s) {
        case SessionState::Stopped: return os << "Stopped";
        case SessionState::Checking: return os << "Checking";
        case SessionState::Downloading: return os << "Downloading";
        case SessionState::Waiting: return os << "Waiting";
        case SessionState::Seeding: return os << "Seeding";
        case SessionState::Paused: return os << "Paused";
        case SessionState::Error: return os << "Error";
    }
    return os << "Unknown";
}

// Thông tin chi tiết một Peer đang kết nối trong Session
struct SessionPeerInfo {
    std::string ip;
    std::uint16_t port = 0;
    std::string peer_id;
    bool am_choking = true;
    bool peer_choking = true;
    bool am_interested = false;
    bool peer_interested = false;
    double download_rate = 0.0;     // bytes/s
    double upload_rate = 0.0;       // bytes/s
    double progress_percent = 0.0;  // 0.0 -> 100.0%
};

// Số liệu thống kê thời gian thực của TorrentSession (phục vụ WebUI, Bench, CLI)
struct TorrentSessionStats {
    SessionState state = SessionState::Stopped;
    std::string state_name = "stopped";
    std::uint64_t total_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t left_bytes = 0;
    double progress_percent = 0.0;
    double download_rate = 0.0;    // bytes/s
    double upload_rate = 0.0;      // bytes/s
    std::size_t total_pieces = 0;
    std::size_t completed_pieces = 0;
    std::size_t connected_peers = 0;
    std::size_t total_peers_known = 0;
    std::size_t dht_nodes_count = 0;
    std::string error_message;
};

// Cấu hình khởi tạo cho TorrentSession
struct TorrentSessionConfig {
    std::filesystem::path download_dir = "./downloads";
    std::uint16_t listen_port = 6881;
    std::size_t max_connections = 30;
    std::chrono::seconds tracker_interval{60};
    std::chrono::seconds choker_interval{10};
    std::chrono::seconds peer_timeout{30};
    bool enable_tracker = true;                 // Cho phép announce tới tracker nếu có URL
    bool enable_lsd = true;                     // Tự tìm kiếm peer qua LSD (BEP 14)
    bool enable_dht = true;                     // Tự tìm kiếm peer qua DHT (BEP 5)
    std::uint16_t dht_port = 0;                 // Port UDP cho DHT (0 = dùng listen_port)
    std::string dht_bootstrap_host;             // Địa chỉ node DHT bootstrap
    std::uint16_t dht_bootstrap_port = 0;       // Cổng node DHT bootstrap
    std::string client_peer_id;                 // Tự sinh nếu để rỗng (-PB0001-...)
};

// Callbacks
using StateChangedCallback = std::function<void(SessionState old_state, SessionState new_state)>;
using PieceCompletedCallback = std::function<void(std::size_t piece_index, std::size_t completed_count, std::size_t total_pieces)>;
using TorrentFinishedCallback = std::function<void()>;

class TorrentSession {
public:
    // Khởi tạo từ TorrentFileMetadata
    TorrentSession(
        const TorrentFileMetadata& metadata,
        const TorrentSessionConfig& config = {});

    // Khởi tạo trực tiếp từ đường dẫn file .torrent
    TorrentSession(
        const std::filesystem::path& torrentFilePath,
        const TorrentSessionConfig& config = {});

    ~TorrentSession();

    // Không copy
    TorrentSession(const TorrentSession&) = delete;
    TorrentSession& operator=(const TorrentSession&) = delete;

    // Điều khiển vòng đời
    bool start();
    void stop();
    void pause();
    void resume();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_complete() const noexcept;
    [[nodiscard]] SessionState state() const noexcept;

    // Quản lý Peer & Discovery
    void add_peer(const std::string& ip, std::uint16_t port);
    void announce_tracker(const std::string& event = "");

    // Thống kê & Dữ liệu
    [[nodiscard]] TorrentSessionStats stats() const;
    [[nodiscard]] std::vector<SessionPeerInfo> get_peers() const;
    [[nodiscard]] const TorrentFileMetadata& metadata() const noexcept;
    [[nodiscard]] std::vector<bool> bitfield() const;
    [[nodiscard]] std::string info_hash_hex() const;
    [[nodiscard]] const std::string& peer_id() const noexcept;
    [[nodiscard]] std::filesystem::path save_path() const;
    [[nodiscard]] KademliaNode* dht() noexcept { return dht_.get(); }

    // Thay đổi chiến lược (Piece Picker & Choker)
    void set_piece_picker(std::unique_ptr<strategy::PiecePicker> picker);
    void set_choker(std::unique_ptr<strategy::Choker> choker);

    // Callbacks
    void on_state_changed(StateChangedCallback cb);
    void on_piece_completed(PieceCompletedCallback cb);
    void on_torrent_finished(TorrentFinishedCallback cb);

private:
    // Cấu hình & Metadata
    TorrentFileMetadata metadata_;
    TorrentSessionConfig config_;
    std::string peer_id_;
    std::string info_hash_raw_;    // 20 bytes binary
    std::string info_hash_hex_;    // 40 ký tự hex
    std::filesystem::path save_path_;
    std::filesystem::path part_path_;  // <save_path>.part: tiến độ tải dở để resume

    // Trạng thái nội bộ
    mutable std::mutex session_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    // Số luồng announce tracker đang chạy nền (detached): destructor phải chờ về 0
    // trước khi huỷ session, nếu không luồng đó sẽ dùng `this` đã bị giải phóng.
    std::atomic<int> announce_inflight_{0};
    SessionState state_{SessionState::Stopped};
    std::string error_message_;

    // Quản lý lưu trữ & Dữ liệu Piece
    std::unique_ptr<PieceManager> piece_manager_;
    std::vector<bool> my_bitfield_;
    std::size_t completed_pieces_count_{0};

    // Tiến trình & Băng thông
    std::uint64_t total_bytes_{0};
    std::uint64_t downloaded_bytes_{0};
    std::uint64_t uploaded_bytes_{0};
    std::uint64_t left_bytes_{0};

    // Bộ tính tốc độ (Moving Average / Delta)
    std::chrono::steady_clock::time_point last_rate_calc_time_;
    std::uint64_t last_downloaded_snapshot_{0};
    std::uint64_t last_uploaded_snapshot_{0};
    double current_download_rate_{0.0};
    double current_upload_rate_{0.0};

    // Chiến lược (Strategy)
    std::unique_ptr<strategy::PiecePicker> piece_picker_;
    std::unique_ptr<strategy::Choker> choker_;

    // Callbacks
    StateChangedCallback state_cb_;
    PieceCompletedCallback piece_cb_;
    TorrentFinishedCallback finished_cb_;

    // Quản lý mạng & Peer Swarm
    struct ActivePeer {
        std::string endpoint_str;
        std::string ip;
        std::uint16_t port = 0;
        std::string remote_peer_id;
        std::shared_ptr<pbl4::network::PeerConnection> connection;
        std::vector<bool> bitfield;
        bool am_choking = true;
        bool peer_choking = true;
        bool am_interested = false;
        bool peer_interested = false;
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t uploaded_bytes = 0;
        double download_rate = 0.0;
        double upload_rate = 0.0;
        std::chrono::steady_clock::time_point last_active;
        std::atomic<bool> worker_active{false};
        std::thread worker_thread;
    };

    std::unordered_map<std::string, std::shared_ptr<ActivePeer>> active_peers_;
    std::unordered_set<std::string> known_peer_endpoints_;
    std::vector<std::string> candidate_peer_endpoints_;

    // Quản lý block đang xin (In-flight block requests)
    struct PendingBlock {
        std::uint32_t piece_index;
        std::uint32_t begin;
        std::uint32_t length;
        std::string peer_endpoint;
        std::chrono::steady_clock::time_point requested_at;
    };
    std::vector<PendingBlock> pending_blocks_;

    // Tracking số block đã nhận cho từng piece đang dở
    struct PieceProgress {
        std::size_t total_blocks = 0;
        std::size_t received_blocks = 0;
        std::vector<bool> received_block_mask;
    };
    std::unordered_map<std::uint32_t, PieceProgress> in_progress_pieces_;

    // Threads chạy nền
    std::thread orchestration_thread_;
    std::thread acceptor_thread_;
    std::unique_ptr<asio::io_context> acceptor_ioc_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;

    // LSD
    std::unique_ptr<LocalServiceDiscovery> lsd_;

    // DHT (BEP 5)
    std::unique_ptr<KademliaNode> dht_;

    // Các hàm nội bộ
    void set_state(SessionState new_state);
    void check_existing_files();
    void orchestration_loop();
    void acceptor_loop();
    void peer_worker_loop(std::shared_ptr<ActivePeer> peer);

    void process_tracker_announce(const std::string& event);
    void perform_choker_update();
    void connect_candidate_peers();
    void cleanup_disconnected_peers();
    void update_rates();

    void handle_peer_message(
        std::shared_ptr<ActivePeer> peer,
        const pbl4::network::PeerMessage& msg);
    void handle_piece_block(
        std::shared_ptr<ActivePeer> peer,
        const pbl4::network::PiecePayload& piece_payload);
    void handle_request_block(
        std::shared_ptr<ActivePeer> peer,
        const pbl4::network::RequestPayload& req_payload);

    bool request_next_blocks(std::shared_ptr<ActivePeer> peer);
    void broadcast_have(std::uint32_t piece_index);
    bool check_and_finalize_completion();
};

}  // namespace p2p::network

namespace p2p {
using TorrentSession = network::TorrentSession;
using SessionState = network::SessionState;
using TorrentSessionStats = network::TorrentSessionStats;
using SessionPeerInfo = network::SessionPeerInfo;
using TorrentSessionConfig = network::TorrentSessionConfig;
}  // namespace p2p
