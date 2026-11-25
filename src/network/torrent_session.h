#pragma once

#include "core/piece_manager.h"
#include "core/torrent_file.h"
#include "network/peer_connection.h"
#include "network/peer_protocol.h"
#include "bench/bandwidth_limiter.h"
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
#include <map>
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

namespace pbl4::network {
class NatTraversal;
class UtpEngine;
class UtpStream;
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
    std::uint64_t wanted_bytes = 0;  // tổng byte của các piece được chọn tải (= total_bytes nếu tải hết)
    double progress_percent = 0.0;   // tính trên phần được chọn
    double download_rate = 0.0;    // bytes/s
    double upload_rate = 0.0;      // bytes/s
    std::size_t total_pieces = 0;
    std::size_t completed_pieces = 0;
    std::size_t connected_peers = 0;
    std::size_t total_peers_known = 0;
    std::size_t dht_nodes_count = 0;
    std::string error_message;
};

// Trạng thái 1 file trong torrent (torrent 1 file có đúng 1 mục).
struct FileStatus {
    std::string path;
    std::uint64_t length = 0;
    bool wanted = true;                  // false = bỏ qua, không tải
    std::uint64_t completed_bytes = 0;   // byte thuộc các piece đã verify
};

// Cấu hình khởi tạo cho TorrentSession
struct TorrentSessionConfig {
    std::filesystem::path download_dir = "./downloads";
    std::uint16_t listen_port = 6881;
    std::size_t max_connections = 30;
    // Kết nối VÀO (do peer lạ mở): mỗi IP tối đa bấy nhiêu kết nối vào cùng lúc (0 = không giới hạn). Nếu không, một máy
    // mở 30 kết nối rồi im lặng sẽ lấp đầy mọi chỗ trong lúc peer thật bị đóng cửa (slowloris).
    std::size_t max_inbound_per_ip = 4;
    // Kết nối vào chỉ được dùng tối đa 80% số chỗ; 20% chừa cho kết nối RA do mình chủ động mở tới peer đã biết.
    // Kết nối vào phải bắt tay xong trong thời gian này, quá hạn bị đóng (sau bắt tay mới dùng peer_timeout).
    std::chrono::seconds handshake_timeout{10};
    std::chrono::seconds tracker_interval{60};
    std::chrono::seconds choker_interval{10};
    std::chrono::seconds peer_timeout{30};
    bool enable_tracker = true;                 // Cho phép announce tới tracker nếu có URL
    bool enable_lsd = true;                     // Tự tìm kiếm peer qua LSD (BEP 14)
    bool enable_dht = true;                     // Tự tìm kiếm peer qua DHT (BEP 5)
    // Mặc định bộ lọc LAN từ chối peer public khi máy ở mạng private (hoặc không biết). Bật cờ này
    // (hoặc đặt biến môi trường P2P_ALLOW_PUBLIC_PEERS=1) cho mạng cấp IP public / muốn nối Internet.
    bool allow_public_peers = false;
    bool enable_port_mapping = true;            // Tự mở cổng peer qua UPnP/NAT-PMP nếu router hỗ trợ
    bool enable_utp = true;                     // uTP + UDP hole-punch fallback qua tracker
    std::uint16_t dht_port = 0;                 // Port UDP cho DHT (0 = dùng listen_port)
    std::string dht_bootstrap_host;             // Địa chỉ node DHT bootstrap
    std::uint16_t dht_bootstrap_port = 0;       // Cổng node DHT bootstrap
    std::shared_ptr<KademliaNode> shared_dht;   // Node DHT dùng chung giữa các session (nếu có)
    std::string client_peer_id;                 // Tự sinh nếu để rỗng (-PB0001-...)
    // Số request block chạy song song trên mỗi peer. Mỗi block 16 KiB nên tốc độ tối đa mỗi peer
    // ~ pipeline_depth x 16 KiB / RTT: 4 chỉ đủ ~1.3 MB/s ở RTT 50 ms, 16 đủ ~5 MB/s.
    std::size_t pipeline_depth = 16;
    std::uint64_t max_download_rate = 0;        // byte/s, 0 = không giới hạn
    std::uint64_t max_upload_rate = 0;          // byte/s, 0 = không giới hạn
    // Giới hạn tổng dùng chung cho mọi session (null = không có). Mỗi block phải đủ token ở CẢ giới hạn riêng của
    // torrent lẫn giới hạn tổng.
    std::shared_ptr<bench::BandwidthLimiter> global_download_limiter;
    std::shared_ptr<bench::BandwidthLimiter> global_upload_limiter;
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
    // Chọn file cần tải (chỉ ý nghĩa với torrent nhiều file; torrent 1 file luôn tải file đó).
    // wanted.size() phải bằng số file và ít nhất 1 file được chọn, nếu không trả false và giữ nguyên.
    // Đổi được khi đang chạy: piece chỉ thuộc file bị bỏ sẽ không được xin nữa.
    bool set_file_selection(const std::vector<bool>& wanted);
    [[nodiscard]] std::vector<bool> file_selection() const;
    [[nodiscard]] std::vector<FileStatus> files_status() const;

    void pause();
    [[nodiscard]] bool is_paused() const noexcept { return paused_.load(); }

    // Giới hạn tốc độ (byte/s, 0 = không giới hạn); đổi được khi đang chạy.
    void set_download_limit(std::uint64_t bytes_per_sec) { download_limiter_.set_rate(bytes_per_sec); }
    void set_upload_limit(std::uint64_t bytes_per_sec) { upload_limiter_.set_rate(bytes_per_sec); }
    [[nodiscard]] std::uint64_t download_limit() const { return download_limiter_.rate(); }
    [[nodiscard]] std::uint64_t upload_limit() const { return upload_limiter_.rate(); }
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
    [[nodiscard]] KademliaNode* dht() noexcept { return dht_shared_ ? dht_shared_.get() : dht_.get(); }
    [[nodiscard]] const KademliaNode* dht() const noexcept { return dht_shared_ ? dht_shared_.get() : dht_.get(); }

    // Thay đổi chiến lược (Piece Picker & Choker)
    void set_piece_picker(std::unique_ptr<strategy::PiecePicker> picker);
    // Đổi chiến lược chọn piece theo tên ("rarest_first" hoặc "sequential") nhưng GIỮ pipeline_depth của session
    // (picker tạo bằng cấu hình mặc định sẽ tụt từ 16 xuống 4 request song song). false nếu tên không hợp lệ.
    bool set_piece_strategy(const std::string& name);
    // Số request song song mỗi peer của picker hiện tại (dùng cho test và chẩn đoán).
    [[nodiscard]] std::size_t piece_pipeline_depth() const;
    void set_choker(std::unique_ptr<strategy::Choker> choker);

    // Callbacks
    void on_state_changed(StateChangedCallback cb);
    void on_piece_completed(PieceCompletedCallback cb);
    void on_torrent_finished(TorrentFinishedCallback cb);

private:
    // Cấu hình & Metadata
    TorrentFileMetadata metadata_;
    TorrentSessionConfig config_;
    std::atomic<std::uint16_t> advertised_port_{0};
    std::string peer_id_;
    std::string info_hash_raw_;    // 20 bytes binary
    std::string info_hash_hex_;    // 40 ký tự hex
    std::filesystem::path save_path_;
    std::filesystem::path part_path_;  // <save_path>.part: tiến độ tải dở để resume

    // Trạng thái nội bộ
    mutable std::mutex session_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    // Chọn file: piece_wanted_[i] = piece i chạm ít nhất 1 file được chọn. Mặc định tải hết.
    std::vector<bool> piece_wanted_;
    std::vector<bool> file_wanted_;
    std::size_t wanted_remaining_ = 0;   // số piece được chọn mà chưa có
    std::uint64_t wanted_bytes_ = 0;
    void recompute_wanted_locked();      // tính lại wanted_remaining_/wanted_bytes_/left_bytes_
    [[nodiscard]] bool all_wanted_done_locked() const {
        return !metadata_.pieces.empty() && wanted_remaining_ == 0;
    }
    bench::BandwidthLimiter download_limiter_;
    bench::BandwidthLimiter upload_limiter_;
    bool consume_download(std::uint64_t bytes) {
        return bench::BandwidthLimiter::consume_both(download_limiter_, config_.global_download_limiter.get(), bytes);
    }
    bool consume_upload(std::uint64_t bytes) {
        return bench::BandwidthLimiter::consume_both(upload_limiter_, config_.global_upload_limiter.get(), bytes);
    }
    // Số luồng announce tracker đang chạy nền (detached): destructor phải chờ về 0
    // trước khi huỷ session, nếu không luồng đó sẽ dùng `this` đã bị giải phóng.
    std::atomic<int> announce_inflight_{0};
    std::thread dht_init_thread_;
    std::thread nat_init_thread_;
    std::atomic<bool> nat_mapping_initialization_done_{false};
    std::shared_ptr<pbl4::network::UtpEngine> utp_engine_;
    asio::ip::udp::endpoint tracker_udp_endpoint_;
    std::chrono::steady_clock::time_point next_udp_registration_;
    // Khoá phiên ngẫu nhiên của session này: gửi tracker (HTTP `key`, hex) và kèm trong gói UDP rendezvous (8 byte thô).
    std::string announce_key_raw_;
    std::string announce_key_hex_;
    // transaction_id ngẫu nhiên 32 bit (không tuần tự): kẻ giả phản hồi của tracker phải đoán đúng cả giá trị này.
    struct PendingRendezvous {
        std::string tcp_endpoint;
        std::chrono::steady_clock::time_point requested_at;
    };
    std::unordered_map<std::uint32_t, PendingRendezvous> pending_rendezvous_targets_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_rendezvous_request_;
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
    // Có nhận thêm kết nối VÀO từ `ip` không. Gọi khi đang giữ session_mutex_.
    [[nodiscard]] bool inbound_allowed_locked(const std::string& ip) const;

    struct ActivePeer {
        std::string endpoint_str;
        std::string ip;
        std::uint16_t port = 0;
        bool outbound = false;
        std::string remote_peer_id;
        std::shared_ptr<pbl4::network::PeerConnection> connection;
        std::vector<bool> bitfield;
        bool am_choking = true;
        bool peer_choking = true;
        bool am_interested = false;
        bool peer_interested = false;
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t uploaded_bytes = 0;
        std::uint64_t remote_downloaded_bytes = 0;
        std::uint64_t remote_uploaded_bytes = 0;
        // BEP 6 Fast Extension: chỉ bật khi CẢ HAI bên đặt bit trong handshake.
        bool fast = false;
        bool throttled = false;  // lần xin gần nhất bị giới hạn băng thông từ chối (chỉ luồng worker của peer đọc/ghi)
        std::unordered_set<std::uint32_t> allowed_fast_to_us;     // peer cho ta xin dù đang choke ta (ghi dưới session_mutex_)
        std::unordered_set<std::uint32_t> allowed_fast_for_peer;  // ta cho peer xin dù đang choke họ (chỉ luồng worker của peer)
        std::unordered_set<std::uint32_t> rejected_pieces;        // peer đã Reject: không xin lại piece này từ họ (dưới session_mutex_)
        // Có thể gửi request cho peer này không (mở khoá bình thường, hoặc còn piece "allowed fast" khi bị choke).
        [[nodiscard]] bool can_request() const {
            return am_interested && (!peer_choking || (fast && !allowed_fast_to_us.empty()));
        }
        std::map<std::string, pbl4::network::Byte> remote_extensions;
        std::unordered_set<std::string> pex_advertised;
        double download_rate = 0.0;
        double upload_rate = 0.0;
        std::chrono::steady_clock::time_point last_active;
        std::atomic<bool> worker_active{false};
        std::thread worker_thread;
    };

    std::unordered_map<std::string, std::shared_ptr<ActivePeer>> active_peers_;
    std::unordered_set<std::string> known_peer_endpoints_;
    std::unordered_set<std::string> discovered_tcp_peer_endpoints_;
    std::vector<std::string> candidate_peer_endpoints_;
    std::vector<std::string> utp_peer_candidates_;
    std::unordered_set<std::string> queued_utp_peer_endpoints_;

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

    // NAT port mapping for inbound peer connections
    std::unique_ptr<pbl4::network::NatTraversal> nat_traversal_;
    bool tcp_port_mapped_{false};
    std::uint16_t mapped_tcp_port_{0};
    std::chrono::steady_clock::time_point next_port_mapping_refresh_;

    // Đổi mạng khi đang chạy (NetworkMonitor đặt cờ, orchestration_loop xử lý)
    std::uint64_t network_subscription_{0};
    std::atomic<bool> network_changed_{false};          // có đổi gì đó (kể cả card phụ)
    std::atomic<bool> network_primary_changed_{false};  // IP chính đổi: kết nối + ánh xạ cũ vô hiệu
    std::atomic<bool> network_full_refresh_{false};     // IP chính đổi hoặc mạng vừa có lại
    // Dò router + ánh xạ cổng TCP; điền nat_traversal_/mapped_tcp_port_/advertised_port_. Chỉ một luồng gọi tại một
    // thời điểm (luồng khởi tạo lúc start, sau đó là orchestration_loop). replace_stale_mapping: nếu router từ chối vì
    // cổng còn ánh xạ cũ thì gỡ rồi ánh xạ lại một lần.
    void establish_port_mapping(bool replace_stale_mapping = false);
    void remap_ports_after_network_change();
    // Xử lý cờ đổi mạng. Trả true nếu đã làm gì đó (đã announce lại tracker/LSD).
    bool handle_network_change();

    // DHT (BEP 5)
    std::shared_ptr<KademliaNode> dht_shared_;
    std::unique_ptr<KademliaNode> dht_;

    // Các hàm nội bộ
    void set_state(SessionState new_state);
    void check_existing_files();
    void orchestration_loop();
    void acceptor_loop();
    void accept_utp_stream(std::shared_ptr<pbl4::network::UtpStream> stream);
    void peer_worker_loop(std::shared_ptr<ActivePeer> peer);

    void process_tracker_announce(const std::string& event);
    void perform_choker_update();
    void connect_candidate_peers();
    void connect_utp_candidates();
    void send_udp_registration();
    void request_peer_holepunch(const std::string& ip, std::uint16_t tcp_port);
    bool handle_rendezvous_datagram(
        const char* data,
        std::size_t size,
        const asio::ip::udp::endpoint& source);
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
    void send_peer_stats(const std::shared_ptr<ActivePeer>& peer);
    void send_peer_exchange(const std::shared_ptr<ActivePeer>& peer);

    // Gửi qua socket có thể chặn tới io_timeout (30 giây) khi peer ngừng đọc (pause, mạng nghẽn). Tuyệt đối không gọi
    // khi đang giữ session_mutex_: mọi thứ cần khoá đó (stats() của Web UI, GET /api/torrents, announce...) sẽ treo theo.
    // Phần tính toán chạy trong khoá và chỉ ghi lại việc cần gửi; việc gửi thật chạy sau khi nhả khoá.
    struct BlockRequest {
        bool valid = false;
        std::uint32_t piece = 0;
        std::uint32_t begin = 0;
        std::uint32_t length = 0;
        bool throttled = false;  // không xin được vì hết băng thông (không phải vì hết việc để xin)
    };
    bool request_next_blocks(std::shared_ptr<ActivePeer> peer);
    // Áp bitfield của peer (Bitfield / HaveAll / HaveNone), bật "interested" nếu họ có piece ta cần.
    void apply_peer_bitfield(const std::shared_ptr<ActivePeer>& peer, const std::vector<bool>& bits);
    bool request_next_blocks_locked(std::shared_ptr<ActivePeer> peer, BlockRequest& out);
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
