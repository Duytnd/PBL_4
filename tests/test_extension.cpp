#include "network/extension.h"
#include "network/magnet_link.h"

#include <cassert>
#include <iostream>

using namespace pbl4::network;

void test_extended_handshake_round_trip() {
	const auto message = extension::make_extended_handshake();
	assert(message.id == static_cast<Byte>(PeerMessageId::Extended));
	assert(!message.payload.empty());
	assert(message.payload[0] == extension::kExtendedHandshakeId);

	const auto mapping = extension::parse_extended_handshake(message);
	assert(mapping.size() == 3);
	assert(mapping.at(extension::kPbStatsName) == extension::kPbStatsId);
	assert(mapping.at(extension::kUtPexName) == extension::kUtPexId);
	assert(mapping.at(extension::kUtMetadataName) == extension::kUtMetadataId);
	assert(!extension::parse_metadata_size(message).has_value());

	const auto metadata_handshake = extension::make_extended_handshake(32768);
	assert(extension::parse_metadata_size(metadata_handshake) == 32768);
}

void test_peer_stats_round_trip_and_id_mapping() {
	const extension::PeerStats expected{123456, 654321};
	const auto message = extension::make_peer_stats(7, expected);
	assert(message.payload[0] == 7);

	const auto parsed = extension::parse_peer_stats(message, 7);
	assert(parsed.has_value());
	assert(*parsed == expected);
	assert(!extension::parse_peer_stats(message, extension::kPbStatsId).has_value());
}

void test_malformed_extension_payloads_are_rejected() {
	bool caught = false;
	try {
		auto malformed = extension::make_extended_handshake();
		malformed.payload = {extension::kExtendedHandshakeId, 'x'};
		(void)extension::parse_extended_handshake(malformed);
	} catch (const std::exception&) {
		caught = true;
	}
	assert(caught);

	caught = false;
	try {
		PeerMessage malformed{false, static_cast<Byte>(PeerMessageId::Extended),
			{extension::kPbStatsId, 'd', 'e'}};
		(void)extension::parse_peer_stats(malformed, extension::kPbStatsId);
	} catch (const std::exception&) {
		caught = true;
	}
	assert(caught);
}

void test_metadata_message_round_trip() {
	const auto request = extension::make_metadata_request(9, 3);
	const auto parsed_request = extension::parse_metadata_message(request, 9);
	assert(parsed_request.type == extension::MetadataMessage::Type::Request);
	assert(parsed_request.piece == 3);

	const Bytes raw_data{0, 0xff, 'm', 'e', 't', 'a'};
	const auto data = extension::make_metadata_data(9, 3, 100, raw_data);
	const auto parsed_data = extension::parse_metadata_message(data, 9);
	assert(parsed_data.type == extension::MetadataMessage::Type::Data);
	assert(parsed_data.piece == 3);
	assert(parsed_data.total_size == 100);
	assert(parsed_data.data == raw_data);

	const auto reject = extension::make_metadata_reject(9, 3);
	assert(extension::parse_metadata_message(reject, 9).type ==
		extension::MetadataMessage::Type::Reject);
}

void test_pex_compact_peer_round_trip() {
	const std::vector<extension::PexPeer> added{{"192.168.1.20", 51413}, {"10.0.0.2", 6881}};
	const std::vector<extension::PexPeer> dropped{{"172.16.0.4", 5000}};
	const auto message = extension::make_pex_message(11, added, dropped);
	const auto parsed = extension::parse_pex_message(message, 11);
	assert(parsed.added == added);
	assert(parsed.dropped == dropped);
}

void test_magnet_uri_parsing() {
	const auto magnet = p2p::network::parse_magnet_uri(
		"magnet:?xt=urn:btih:0123456789ABCDEF0123456789abcdef01234567"
		"&dn=hello+world&tr=http%3A%2F%2Ftracker.example%3A6969%2Fannounce");
	assert(magnet.info_hash.size() == 20);
	assert(magnet.info_hash_hex == "0123456789abcdef0123456789abcdef01234567");
	assert(magnet.display_name == "hello world");
	assert(magnet.trackers.size() == 1);
	assert(magnet.trackers[0] == "http://tracker.example:6969/announce");

	const auto base32 = p2p::network::parse_magnet_uri(
		"magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	assert(base32.info_hash == std::string(20, '\0'));

	bool caught = false;
	try {
		(void)p2p::network::parse_magnet_uri("magnet:?dn=missing-hash");
	} catch (const std::exception&) {
		caught = true;
	}
	assert(caught);
}

int main() {
	test_extended_handshake_round_trip();
	test_peer_stats_round_trip_and_id_mapping();
	test_metadata_message_round_trip();
	test_pex_compact_peer_round_trip();
	test_magnet_uri_parsing();
	test_malformed_extension_payloads_are_rejected();
	std::cout << "All extension tests passed.\n";
	return 0;
}
