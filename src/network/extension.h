#pragma once

#include "network/peer_protocol.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbl4::network::extension {

inline constexpr Byte kExtendedHandshakeId = 0;
inline constexpr Byte kPbStatsId = 1;
inline constexpr Byte kUtPexId = 2;
inline constexpr Byte kUtMetadataId = 3;
inline constexpr char kPbStatsName[] = "pb_stats";
inline constexpr char kUtPexName[] = "ut_pex";
inline constexpr char kUtMetadataName[] = "ut_metadata";
inline constexpr std::size_t kMetadataBlockSize = 16 * 1024;
inline constexpr std::size_t kMaxMetadataSize = 4 * 1024 * 1024;

using ExtensionMap = std::map<std::string, Byte>;

struct PeerStats {
	std::uint64_t downloaded = 0;
	std::uint64_t uploaded = 0;

	bool operator==(const PeerStats& other) const noexcept {
		return downloaded == other.downloaded && uploaded == other.uploaded;
	}
};

struct MetadataMessage {
	enum class Type { Request, Data, Reject };

	Type type = Type::Reject;
	std::uint32_t piece = 0;
	std::uint32_t total_size = 0;
	Bytes data;
};

struct PexPeer {
	std::string ip;
	std::uint16_t port = 0;

	bool operator==(const PexPeer& other) const noexcept {
		return ip == other.ip && port == other.port;
	}
};

struct PexMessage {
	std::vector<PexPeer> added;
	std::vector<PexPeer> dropped;
};

PeerMessage make_extended_handshake(
	std::optional<std::uint32_t> metadata_size = std::nullopt);
ExtensionMap parse_extended_handshake(const PeerMessage& message);

PeerMessage make_peer_stats(Byte recipient_extension_id, const PeerStats& stats);
std::optional<PeerStats> parse_peer_stats(
	const PeerMessage& message,
	Byte local_extension_id);

std::optional<std::uint32_t> parse_metadata_size(const PeerMessage& message);
PeerMessage make_metadata_request(Byte recipient_extension_id, std::uint32_t piece);
PeerMessage make_metadata_data(
	Byte recipient_extension_id,
	std::uint32_t piece,
	std::uint32_t total_size,
	const Bytes& data);
PeerMessage make_metadata_reject(Byte recipient_extension_id, std::uint32_t piece);
MetadataMessage parse_metadata_message(const PeerMessage& message, Byte local_extension_id);

PeerMessage make_pex_message(
	Byte recipient_extension_id,
	const std::vector<PexPeer>& added,
	const std::vector<PexPeer>& dropped = {});
PexMessage parse_pex_message(const PeerMessage& message, Byte local_extension_id);

}  // namespace pbl4::network::extension
