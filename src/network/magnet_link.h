#pragma once

#include <string>
#include <vector>

namespace p2p::network {

struct MagnetLink {
	std::string info_hash;
	std::string info_hash_hex;
	std::string display_name;
	std::vector<std::string> trackers;
};

MagnetLink parse_magnet_uri(const std::string& uri);

}  // namespace p2p::network