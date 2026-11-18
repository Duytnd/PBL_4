#pragma once

#include "core/torrent_file.h"
#include "network/magnet_link.h"
#include "network/tracker_protocol.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace p2p::network {

struct MagnetResolveOptions {
	std::string peer_id;
	std::uint16_t listen_port = 0;
	bool enable_trackers = true;
	bool enable_dht = true;
	std::string dht_bootstrap_host;
	std::uint16_t dht_bootstrap_port = 0;
	std::uint16_t dht_port = 0;
	std::size_t max_peer_attempts = 20;
	// Chưa lấy được metadata (tracker chưa có peer, seeder vừa khởi động, peer đầu tiên lỗi...) thì hỏi lại
	// mỗi retry_interval cho tới hết retry_timeout. Chỉ áp dụng khi còn nguồn để thử lại (tracker trong magnet
	// hoặc DHT); không có nguồn nào thì báo lỗi ngay. retry_timeout = 0 tắt việc thử lại.
	std::chrono::milliseconds retry_timeout{45000};
	std::chrono::milliseconds retry_interval{3000};
	// Mỗi lượt chỉ được tốn tối đa chừng này để thử lần lượt các peer (mỗi peer lỗi có thể chờ vài giây). Nếu không,
	// 20 peer ứng viên có thể mất cả phút và request WebUI hết hạn trước khi resolver trả lời.
	std::chrono::milliseconds peer_fetch_budget{20000};
	// Mặc định theo chính sách LAN của dự án: khi có peer nội bộ thì bỏ peer public (DHT hay trả cả IP public của
	// chính router hoặc peer lạ ngoài Internet, chỉ tốn thời gian kết nối quá hạn). Bật để nối swarm Internet
	// (cũng bật được bằng biến môi trường P2P_ALLOW_PUBLIC_PEERS=1).
	bool allow_public_peers = false;
};

TorrentFileMetadata resolve_magnet_uri(
	const std::string& uri,
	const MagnetResolveOptions& options);

TorrentFileMetadata fetch_magnet_metadata_from_peers(
	const MagnetLink& magnet,
	const std::vector<TrackerPeer>& peers,
	const std::string& peer_id,
	const std::string& announce_url = {},
	std::chrono::milliseconds budget = std::chrono::seconds(20));

}  // namespace p2p::network