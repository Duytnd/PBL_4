#include "network/extension.h"

#include "core/bencode.h"

#include <charconv>
#include <limits>
#include <stdexcept>

namespace pbl4::network::extension {
namespace {

using p2p::BencodeDict;
using p2p::BencodeValue;

constexpr std::size_t kMaxPexPeers = 200;

Bytes to_bytes(const std::string& value) {
	return Bytes(value.begin(), value.end());
}

std::string to_string(const Bytes& value, std::size_t offset = 0) {
	return std::string(value.begin() + static_cast<std::ptrdiff_t>(offset), value.end());
}

std::int64_t to_bencode_integer(std::uint64_t value) {
	if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		throw std::overflow_error("pb_stats value exceeds the bencode integer range");
	}
	return static_cast<std::int64_t>(value);
}

std::uint64_t read_nonnegative_integer(const BencodeDict& dict, const char* key) {
	const auto item = dict.find(key);
	if (item == dict.end()) {
		throw std::invalid_argument(std::string("pb_stats missing field: ") + key);
	}
	const auto* value = item->second.get_if<std::int64_t>();
	if (value == nullptr || *value < 0) {
		throw std::invalid_argument(std::string("pb_stats field must be a nonnegative integer: ") + key);
	}
	return static_cast<std::uint64_t>(*value);
}

std::uint32_t read_u32(const BencodeDict& dict, const char* key) {
	const auto item = dict.find(key);
	if (item == dict.end()) {
		throw std::invalid_argument(std::string("extension payload missing field: ") + key);
	}
	const auto* value = item->second.get_if<std::int64_t>();
	if (value == nullptr || *value < 0 ||
		static_cast<std::uint64_t>(*value) > std::numeric_limits<std::uint32_t>::max()) {
		throw std::invalid_argument(std::string("extension field is outside uint32 range: ") + key);
	}
	return static_cast<std::uint32_t>(*value);
}

std::string compact_peers(const std::vector<PexPeer>& peers) {
	if (peers.size() > kMaxPexPeers) {
		throw std::invalid_argument("PEX peer list exceeds the configured limit");
	}

	std::string compact;
	compact.reserve(peers.size() * 6);
	for (const auto& peer : peers) {
		if (peer.port == 0) {
			throw std::invalid_argument("PEX peer port must not be zero");
		}
		std::size_t start = 0;
		for (int octet = 0; octet < 4; ++octet) {
			const std::size_t end = octet == 3 ? peer.ip.size() : peer.ip.find('.', start);
			if (end == std::string::npos || end == start) {
				throw std::invalid_argument("PEX supports only valid IPv4 addresses");
			}
			unsigned int value = 0;
			const auto result = std::from_chars(peer.ip.data() + start, peer.ip.data() + end, value);
			if (result.ec != std::errc{} || result.ptr != peer.ip.data() + end || value > 255) {
				throw std::invalid_argument("PEX contains an invalid IPv4 address");
			}
			compact.push_back(static_cast<char>(value));
			start = end + 1;
		}
		if (start != peer.ip.size() + 1) {
			throw std::invalid_argument("PEX contains an invalid IPv4 address");
		}
		compact.push_back(static_cast<char>((peer.port >> 8) & 0xff));
		compact.push_back(static_cast<char>(peer.port & 0xff));
	}
	return compact;
}

std::vector<PexPeer> parse_compact_peers(const std::string& compact) {
	if (compact.size() % 6 != 0 || compact.size() / 6 > kMaxPexPeers) {
		throw PeerProtocolError("PEX compact peer list has an invalid length");
	}

	std::vector<PexPeer> peers;
	peers.reserve(compact.size() / 6);
	for (std::size_t offset = 0; offset < compact.size(); offset += 6) {
		const auto byte = [&](std::size_t index) {
			return static_cast<unsigned int>(static_cast<unsigned char>(compact[offset + index]));
		};
		const std::uint16_t port = static_cast<std::uint16_t>((byte(4) << 8) | byte(5));
		if (port == 0) {
			continue;
		}
		peers.push_back({std::to_string(byte(0)) + "." + std::to_string(byte(1)) + "." +
			std::to_string(byte(2)) + "." + std::to_string(byte(3)), port});
	}
	return peers;
}

BencodeDict parse_extension_dict(const PeerMessage& message, Byte local_extension_id,
	std::size_t& payload_offset) {
	if (message.id != static_cast<Byte>(PeerMessageId::Extended) ||
		message.payload.empty() || message.payload[0] != local_extension_id ||
		local_extension_id == kExtendedHandshakeId) {
		throw PeerProtocolError("message has an unexpected extension ID");
	}
	std::size_t consumed = 0;
	const auto decoded = p2p::decodeBencode(to_string(message.payload, 1), &consumed);
	const auto* dict = decoded.get_if<BencodeDict>();
	if (dict == nullptr) {
		throw PeerProtocolError("extension header must be a bencoded dictionary");
	}
	payload_offset = consumed + 1;
	return *dict;
}

}  // namespace

PeerMessage make_extended_handshake(std::optional<std::uint32_t> metadata_size) {
	BencodeDict extensions;
	extensions.emplace(kPbStatsName, BencodeValue(static_cast<std::int64_t>(kPbStatsId)));
	extensions.emplace(kUtPexName, BencodeValue(static_cast<std::int64_t>(kUtPexId)));
	extensions.emplace(kUtMetadataName, BencodeValue(static_cast<std::int64_t>(kUtMetadataId)));

	BencodeDict handshake;
	handshake.emplace("m", BencodeValue(std::move(extensions)));
	handshake.emplace("v", BencodeValue("PBL4 1.0"));
	if (metadata_size) {
		if (*metadata_size == 0 || *metadata_size > kMaxMetadataSize) {
			throw std::invalid_argument("metadata_size is outside the configured limit");
		}
		handshake.emplace("metadata_size", BencodeValue(static_cast<std::int64_t>(*metadata_size)));
	}
	return peer_protocol::make_extended(kExtendedHandshakeId,
		to_bytes(p2p::encodeBencode(handshake)));
}

ExtensionMap parse_extended_handshake(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Extended) ||
		message.payload.empty() || message.payload[0] != kExtendedHandshakeId) {
		throw PeerProtocolError("message is not an extended handshake");
	}

	const auto value = p2p::decodeBencode(to_string(message.payload, 1));
	const auto* dict = value.get_if<BencodeDict>();
	if (dict == nullptr) {
		throw PeerProtocolError("extended handshake must be a bencoded dictionary");
	}

	const auto mapping = dict->find("m");
	if (mapping == dict->end() || mapping->second.get_if<BencodeDict>() == nullptr) {
		throw PeerProtocolError("extended handshake must contain an extension map");
	}

	ExtensionMap result;
	for (const auto& [name, id_value] : *mapping->second.get_if<BencodeDict>()) {
		const auto* id = id_value.get_if<std::int64_t>();
		if (name.empty() || id == nullptr || *id < 0 || *id > 255) {
			throw PeerProtocolError("extended handshake contains an invalid extension mapping");
		}
		result.emplace(name, static_cast<Byte>(*id));
	}
	return result;
}

PeerMessage make_peer_stats(Byte recipient_extension_id, const PeerStats& stats) {
	if (recipient_extension_id == kExtendedHandshakeId) {
		throw std::invalid_argument("extension ID zero is reserved for the extended handshake");
	}

	BencodeDict payload;
	payload.emplace("downloaded", BencodeValue(to_bencode_integer(stats.downloaded)));
	payload.emplace("uploaded", BencodeValue(to_bencode_integer(stats.uploaded)));
	return peer_protocol::make_extended(recipient_extension_id,
		to_bytes(p2p::encodeBencode(payload)));
}

std::optional<PeerStats> parse_peer_stats(
	const PeerMessage& message,
	Byte local_extension_id) {
	if (message.id != static_cast<Byte>(PeerMessageId::Extended) ||
		message.payload.empty() || message.payload[0] != local_extension_id ||
		local_extension_id == kExtendedHandshakeId) {
		return std::nullopt;
	}

	const auto value = p2p::decodeBencode(to_string(message.payload, 1));
	const auto* dict = value.get_if<BencodeDict>();
	if (dict == nullptr) {
		throw PeerProtocolError("pb_stats payload must be a bencoded dictionary");
	}

	return PeerStats{
		read_nonnegative_integer(*dict, "downloaded"),
		read_nonnegative_integer(*dict, "uploaded")};
}

std::optional<std::uint32_t> parse_metadata_size(const PeerMessage& message) {
	if (message.id != static_cast<Byte>(PeerMessageId::Extended) ||
		message.payload.empty() || message.payload[0] != kExtendedHandshakeId) {
		throw PeerProtocolError("message is not an extended handshake");
	}
	const auto value = p2p::decodeBencode(to_string(message.payload, 1));
	const auto* dict = value.get_if<BencodeDict>();
	if (dict == nullptr) {
		throw PeerProtocolError("extended handshake must be a bencoded dictionary");
	}
	const auto size = dict->find("metadata_size");
	if (size == dict->end()) {
		return std::nullopt;
	}
	const auto* integer = size->second.get_if<std::int64_t>();
	if (integer == nullptr || *integer <= 0 ||
		static_cast<std::uint64_t>(*integer) > kMaxMetadataSize) {
		throw PeerProtocolError("extended handshake metadata_size is invalid");
	}
	return static_cast<std::uint32_t>(*integer);
}

PeerMessage make_metadata_request(Byte recipient_extension_id, std::uint32_t piece) {
	if (recipient_extension_id == 0) {
		throw std::invalid_argument("metadata extension ID must not be zero");
	}
	BencodeDict header;
	header.emplace("msg_type", BencodeValue(std::int64_t(0)));
	header.emplace("piece", BencodeValue(static_cast<std::int64_t>(piece)));
	return peer_protocol::make_extended(recipient_extension_id,
		to_bytes(p2p::encodeBencode(header)));
}

PeerMessage make_metadata_data(
	Byte recipient_extension_id,
	std::uint32_t piece,
	std::uint32_t total_size,
	const Bytes& data) {
	if (recipient_extension_id == 0 || total_size == 0 || total_size > kMaxMetadataSize ||
		data.empty() || data.size() > kMetadataBlockSize) {
		throw std::invalid_argument("metadata data response has invalid size or extension ID");
	}
	BencodeDict header;
	header.emplace("msg_type", BencodeValue(std::int64_t(1)));
	header.emplace("piece", BencodeValue(static_cast<std::int64_t>(piece)));
	header.emplace("total_size", BencodeValue(static_cast<std::int64_t>(total_size)));
	std::string payload = p2p::encodeBencode(header);
	payload.append(reinterpret_cast<const char*>(data.data()), data.size());
	return peer_protocol::make_extended(recipient_extension_id, to_bytes(payload));
}

PeerMessage make_metadata_reject(Byte recipient_extension_id, std::uint32_t piece) {
	if (recipient_extension_id == 0) {
		throw std::invalid_argument("metadata extension ID must not be zero");
	}
	BencodeDict header;
	header.emplace("msg_type", BencodeValue(std::int64_t(2)));
	header.emplace("piece", BencodeValue(static_cast<std::int64_t>(piece)));
	return peer_protocol::make_extended(recipient_extension_id,
		to_bytes(p2p::encodeBencode(header)));
}

MetadataMessage parse_metadata_message(const PeerMessage& message, Byte local_extension_id) {
	std::size_t data_offset = 0;
	const auto header = parse_extension_dict(message, local_extension_id, data_offset);
	const std::uint32_t type = read_u32(header, "msg_type");
	MetadataMessage parsed;
	parsed.piece = read_u32(header, "piece");
	if (type == 0) {
		if (data_offset != message.payload.size()) {
			throw PeerProtocolError("metadata request must not contain trailing data");
		}
		parsed.type = MetadataMessage::Type::Request;
		return parsed;
	}
	if (type == 2) {
		if (data_offset != message.payload.size()) {
			throw PeerProtocolError("metadata reject must not contain trailing data");
		}
		parsed.type = MetadataMessage::Type::Reject;
		return parsed;
	}
	if (type != 1) {
		throw PeerProtocolError("metadata message type is unsupported");
	}
	parsed.total_size = read_u32(header, "total_size");
	if (parsed.total_size == 0 || parsed.total_size > kMaxMetadataSize ||
		data_offset >= message.payload.size() ||
		message.payload.size() - data_offset > kMetadataBlockSize) {
		throw PeerProtocolError("metadata data response has an invalid size");
	}
	parsed.type = MetadataMessage::Type::Data;
	parsed.data.assign(message.payload.begin() + static_cast<std::ptrdiff_t>(data_offset),
		message.payload.end());
	return parsed;
}

PeerMessage make_pex_message(
	Byte recipient_extension_id,
	const std::vector<PexPeer>& added,
	const std::vector<PexPeer>& dropped) {
	if (recipient_extension_id == 0) {
		throw std::invalid_argument("PEX extension ID must not be zero");
	}
	BencodeDict payload;
	const std::string compact_added = compact_peers(added);
	const std::string compact_dropped = compact_peers(dropped);
	payload.emplace("added", BencodeValue(compact_added));
	payload.emplace("added.f", BencodeValue(std::string(added.size(), '\0')));
	payload.emplace("dropped", BencodeValue(compact_dropped));
	return peer_protocol::make_extended(recipient_extension_id,
		to_bytes(p2p::encodeBencode(payload)));
}

PexMessage parse_pex_message(const PeerMessage& message, Byte local_extension_id) {
	std::size_t data_offset = 0;
	const auto payload = parse_extension_dict(message, local_extension_id, data_offset);
	if (data_offset != message.payload.size()) {
		throw PeerProtocolError("PEX message must not contain trailing data");
	}
	const auto added = payload.find("added");
	if (added == payload.end() || added->second.get_if<std::string>() == nullptr) {
		throw PeerProtocolError("PEX message must contain compact IPv4 peers");
	}
	const auto& compact_added = *added->second.get_if<std::string>();
	const auto flags = payload.find("added.f");
	if (flags != payload.end() &&
		(flags->second.get_if<std::string>() == nullptr ||
		 flags->second.get_if<std::string>()->size() != compact_added.size() / 6)) {
		throw PeerProtocolError("PEX peer flags do not match the added peer count");
	}
	PexMessage result;
	result.added = parse_compact_peers(compact_added);
	const auto dropped = payload.find("dropped");
	if (dropped != payload.end()) {
		const auto* compact_dropped = dropped->second.get_if<std::string>();
		if (compact_dropped == nullptr) {
			throw PeerProtocolError("PEX dropped field must be a compact peer string");
		}
		result.dropped = parse_compact_peers(*compact_dropped);
	}
	return result;
}

}  // namespace pbl4::network::extension
