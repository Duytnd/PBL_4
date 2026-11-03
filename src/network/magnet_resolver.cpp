#include "network/magnet_resolver.h"

#include "core/bencode.h"
#include "core/hash_utils.h"
#include "network/dht/kademlia_node.h"
#include "network/extension.h"
#include "network/peer_connection.h"
#include "network/tracker_client.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <stdexcept>

namespace p2p::network {
namespace {

constexpr std::size_t kMaxMetadataPeers = 20;
constexpr std::size_t kMaxMessagesWhileWaiting = 32;

std::vector<TrackerPeer> unique_peers(const std::vector<TrackerPeer>& peers) {
	std::vector<TrackerPeer> result;
	std::set<std::pair<std::string, std::uint16_t>> seen;
	for (const auto& peer : peers) {
		if (peer.ip.empty() || peer.port == 0 || !seen.emplace(peer.ip, peer.port).second) {
			continue;
		}
		result.push_back(peer);
		if (result.size() >= kMaxMetadataPeers) {
			break;
		}
	}
	return result;
}

std::optional<PeerMessage> receive_metadata_response(
	PeerConnection& connection,
	std::uint32_t expected_piece) {
	for (std::size_t attempt = 0; attempt < kMaxMessagesWhileWaiting; ++attempt) {
		const auto message = connection.receive_message();
		if (message.id != static_cast<Byte>(PeerMessageId::Extended) || message.payload.empty()) {
			continue;
		}
		if (message.payload[0] == extension::kExtendedHandshakeId) {
			continue;
		}
		if (message.payload[0] != extension::kUtMetadataId) {
			continue;
		}
		const auto metadata = extension::parse_metadata_message(message, extension::kUtMetadataId);
		if (metadata.piece == expected_piece) {
			return message;
		}
	}
	return std::nullopt;
}

TorrentFileMetadata request_metadata_from_peer(
	const MagnetLink& magnet,
	const TrackerPeer& peer,
	const std::string& peer_id,
	const std::string& announce_url) {
	PeerConnection connection = PeerConnection::connect_to(peer.ip, peer.port, std::chrono::seconds(5));
	PeerHandshake local_handshake;
	std::memcpy(local_handshake.info_hash.data(), magnet.info_hash.data(), magnet.info_hash.size());
	if (peer_id.size() != local_handshake.peer_id.size()) {
		throw std::invalid_argument("magnet resolver peer_id must be exactly 20 bytes");
	}
	std::memcpy(local_handshake.peer_id.data(), peer_id.data(), peer_id.size());
	local_handshake.set_supports_extension(true);
	connection.send_handshake(local_handshake);

	const auto remote_handshake = connection.receive_handshake();
	if (remote_handshake.info_hash != local_handshake.info_hash) {
		throw std::runtime_error("peer returned a different info_hash");
	}
	if (!remote_handshake.supports_extension()) {
		throw std::runtime_error("peer does not support BEP 10");
	}
	connection.send_message(extension::make_extended_handshake());

	extension::ExtensionMap remote_extensions;
	std::optional<std::uint32_t> metadata_size;
	for (std::size_t attempt = 0; attempt < kMaxMessagesWhileWaiting; ++attempt) {
		const auto message = connection.receive_message();
		if (message.id != static_cast<Byte>(PeerMessageId::Extended) || message.payload.empty() ||
			message.payload[0] != extension::kExtendedHandshakeId) {
			continue;
		}
		remote_extensions = extension::parse_extended_handshake(message);
		metadata_size = extension::parse_metadata_size(message);
		break;
	}

	const auto metadata_id = remote_extensions.find(extension::kUtMetadataName);
	if (metadata_id == remote_extensions.end() || metadata_id->second == 0 || !metadata_size) {
		throw std::runtime_error("peer does not offer BEP 9 metadata");
	}

	const std::size_t block_count =
		(*metadata_size + extension::kMetadataBlockSize - 1) / extension::kMetadataBlockSize;
	std::string raw_info;
	raw_info.reserve(*metadata_size);
	for (std::size_t piece = 0; piece < block_count; ++piece) {
		connection.send_message(extension::make_metadata_request(
			metadata_id->second, static_cast<std::uint32_t>(piece)));

		std::optional<PeerMessage> response = receive_metadata_response(
			connection, static_cast<std::uint32_t>(piece));
		if (!response) {
			throw std::runtime_error("peer did not return requested metadata piece");
		}
		const auto decoded = extension::parse_metadata_message(*response, extension::kUtMetadataId);
		if (decoded.type != extension::MetadataMessage::Type::Data ||
			decoded.total_size != *metadata_size) {
			throw std::runtime_error("peer rejected metadata or changed metadata_size");
		}
		const std::size_t expected_size = std::min<std::size_t>(
			extension::kMetadataBlockSize,
			static_cast<std::size_t>(*metadata_size) - piece * extension::kMetadataBlockSize);
		if (decoded.data.size() != expected_size) {
			throw std::runtime_error("peer returned a metadata piece with an invalid size");
		}
		raw_info.append(reinterpret_cast<const char*>(decoded.data.data()), decoded.data.size());
	}

	if (raw_info.size() != *metadata_size || sha1Digest(raw_info) != magnet.info_hash) {
		throw std::runtime_error("metadata info_hash verification failed");
	}
	const auto decoded_info = decodeBencode(raw_info);
	if (decoded_info.get_if<BencodeDict>() == nullptr || encodeBencode(decoded_info) != raw_info) {
		throw std::runtime_error("metadata info dictionary is malformed or non-canonical");
	}

	const std::string encoded_torrent = "d8:announce" + encodeBencode(announce_url) +
		"4:info" + raw_info + "e";
	TorrentFileMetadata metadata = TorrentFile::parse(encoded_torrent);
	if (metadata.infoRaw != raw_info) {
		throw std::runtime_error("parsed metadata changed the info dictionary bytes");
	}
	return metadata;
}

}  // namespace

TorrentFileMetadata fetch_magnet_metadata_from_peers(
	const MagnetLink& magnet,
	const std::vector<TrackerPeer>& peers,
	const std::string& peer_id,
	const std::string& announce_url) {
	if (magnet.info_hash.size() != 20) {
		throw std::invalid_argument("magnet info_hash must contain 20 bytes");
	}
	if (peer_id.size() != 20) {
		throw std::invalid_argument("magnet resolver peer_id must contain 20 bytes");
	}

	std::string errors;
	for (const auto& peer : unique_peers(peers)) {
		try {
			return request_metadata_from_peer(magnet, peer, peer_id, announce_url);
		} catch (const std::exception& error) {
			if (!errors.empty()) errors += "; ";
			errors += peer.ip + ":" + std::to_string(peer.port) + " " + error.what();
		}
	}
	throw std::runtime_error(errors.empty() ? "no peers available for magnet metadata" : errors);
}

TorrentFileMetadata resolve_magnet_uri(
	const std::string& uri,
	const MagnetResolveOptions& options) {
	if (options.listen_port == 0) {
		throw std::invalid_argument("magnet resolver requires a listening port");
	}
	if (options.max_peer_attempts == 0 || options.max_peer_attempts > kMaxMetadataPeers) {
		throw std::invalid_argument("magnet resolver peer attempt limit is invalid");
	}
	const MagnetLink magnet = parse_magnet_uri(uri);
	const std::string peer_id = options.peer_id.empty()
		? TrackerClient::makePeerId()
		: options.peer_id;
	if (peer_id.size() != 20) {
		throw std::invalid_argument("magnet resolver peer_id must be exactly 20 bytes");
	}

	std::vector<TrackerPeer> peers;
	std::string announce_url;
	for (const auto& tracker : magnet.trackers) {
		if (tracker.rfind("http://", 0) == 0) {
			announce_url = tracker;
			break;
		}
	}

	for (const auto& tracker : magnet.trackers) {
		if (!options.enable_trackers) break;
		try {
			TrackerRequest request;
			request.infoHash = magnet.info_hash;
			request.peerId = peer_id;
			request.port = options.listen_port;
			request.left = 1;
			request.event = "started";
			request.numwant = 50;
			request.compact = true;
			const auto response = TrackerClient::announceHttp(tracker, request);
			if (response.success) {
				peers.insert(peers.end(), response.peers.begin(), response.peers.end());
			}
		} catch (...) {
			// Other trackers and DHT may still supply usable peers.
		}
		if (unique_peers(peers).size() >= options.max_peer_attempts) break;
	}

	std::vector<TrackerPeer> tracker_candidates = unique_peers(peers);
	if (tracker_candidates.size() > options.max_peer_attempts) {
		tracker_candidates.resize(options.max_peer_attempts);
	}
	std::string tracker_error;
	if (!tracker_candidates.empty()) {
		try {
			return fetch_magnet_metadata_from_peers(
				magnet, tracker_candidates, peer_id, announce_url);
		} catch (const std::exception& error) {
			tracker_error = error.what();
		}
	}

	if (!options.enable_dht || options.dht_bootstrap_host.empty() ||
		options.dht_bootstrap_port == 0) {
		throw std::runtime_error(tracker_error.empty()
			? "no tracker peers available for magnet metadata"
			: tracker_error);
	}

	const std::uint16_t dht_port = options.dht_port == 0 ? options.listen_port : options.dht_port;
	KademliaNode dht(NodeId::random(), dht_port);
	if (!dht.start()) {
		throw std::runtime_error(tracker_error.empty()
			? "could not start DHT for magnet peer discovery"
			: tracker_error + "; DHT could not start");
	}

	std::vector<TrackerPeer> dht_candidates;
	try {
		dht.bootstrap(options.dht_bootstrap_host, options.dht_bootstrap_port);
		for (const auto& peer : dht.getPeers(magnet.info_hash)) {
			dht_candidates.push_back({peer.ip, peer.port});
		}
	} catch (const std::exception& error) {
		dht.stop();
		throw std::runtime_error(tracker_error.empty()
			? error.what()
			: tracker_error + "; DHT lookup failed: " + error.what());
	}
	dht.stop();

	std::set<std::pair<std::string, std::uint16_t>> tracker_endpoints;
	for (const auto& peer : tracker_candidates) {
		tracker_endpoints.emplace(peer.ip, peer.port);
	}
	dht_candidates.erase(std::remove_if(dht_candidates.begin(), dht_candidates.end(),
		[&](const TrackerPeer& peer) {
			return tracker_endpoints.find({peer.ip, peer.port}) != tracker_endpoints.end();
		}), dht_candidates.end());
	dht_candidates = unique_peers(dht_candidates);
	if (dht_candidates.size() > options.max_peer_attempts) {
		dht_candidates.resize(options.max_peer_attempts);
	}
	try {
		return fetch_magnet_metadata_from_peers(magnet, dht_candidates, peer_id, announce_url);
	} catch (const std::exception& error) {
		throw std::runtime_error(tracker_error.empty()
			? error.what()
			: tracker_error + "; DHT peers failed: " + error.what());
	}
}

}  // namespace p2p::network