#pragma once

#include "core/torrent_file.h"
#include "network/magnet_link.h"
#include "network/tracker_protocol.h"

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
};

TorrentFileMetadata resolve_magnet_uri(
	const std::string& uri,
	const MagnetResolveOptions& options);

TorrentFileMetadata fetch_magnet_metadata_from_peers(
	const MagnetLink& magnet,
	const std::vector<TrackerPeer>& peers,
	const std::string& peer_id,
	const std::string& announce_url = {});

}  // namespace p2p::network