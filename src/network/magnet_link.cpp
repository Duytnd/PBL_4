#include "network/magnet_link.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>

namespace p2p::network {
namespace {

int hex_value(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

std::string percent_decode(const std::string& value) {
	std::string decoded;
	decoded.reserve(value.size());
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '+') {
			decoded.push_back(' ');
		} else if (value[i] == '%') {
			if (i + 2 >= value.size()) {
				throw std::invalid_argument("magnet URI contains an incomplete percent escape");
			}
			const int high = hex_value(value[i + 1]);
			const int low = hex_value(value[i + 2]);
			if (high < 0 || low < 0) {
				throw std::invalid_argument("magnet URI contains an invalid percent escape");
			}
			decoded.push_back(static_cast<char>((high << 4) | low));
			i += 2;
		} else {
			decoded.push_back(value[i]);
		}
	}
	return decoded;
}

std::string decode_btih(const std::string& value) {
	if (value.size() == 40) {
		std::string raw;
		raw.reserve(20);
		for (std::size_t i = 0; i < value.size(); i += 2) {
			const int high = hex_value(value[i]);
			const int low = hex_value(value[i + 1]);
			if (high < 0 || low < 0) {
				throw std::invalid_argument("magnet BTIH must be 40 hexadecimal characters");
			}
			raw.push_back(static_cast<char>((high << 4) | low));
		}
		return raw;
	}

	if (value.size() == 32) {
		std::string raw;
		raw.reserve(20);
		std::uint32_t buffer = 0;
		int bits = 0;
		for (unsigned char ch : value) {
			ch = static_cast<unsigned char>(std::toupper(ch));
			int digit = ch >= 'A' && ch <= 'Z' ? ch - 'A' :
				(ch >= '2' && ch <= '7' ? ch - '2' + 26 : -1);
			if (digit < 0) {
				throw std::invalid_argument("magnet BTIH must be valid base32 or hexadecimal");
			}
			buffer = (buffer << 5) | static_cast<std::uint32_t>(digit);
			bits += 5;
			if (bits >= 8) {
				bits -= 8;
				raw.push_back(static_cast<char>((buffer >> bits) & 0xff));
			}
		}
		if (raw.size() != 20 || (buffer & ((1u << bits) - 1u)) != 0) {
			throw std::invalid_argument("magnet BTIH has invalid base32 padding");
		}
		return raw;
	}
	throw std::invalid_argument("magnet BTIH must be 40 hex or 32 base32 characters");
}

std::string to_hex(const std::string& raw) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(raw.size() * 2);
	for (unsigned char byte : raw) {
		hex.push_back(digits[byte >> 4]);
		hex.push_back(digits[byte & 0x0f]);
	}
	return hex;
}

}  // namespace

MagnetLink parse_magnet_uri(const std::string& uri) {
	if (uri.size() > 16384 || uri.rfind("magnet:?", 0) != 0) {
		throw std::invalid_argument("expected a magnet:? URI within the size limit");
	}

	MagnetLink result;
	std::size_t cursor = 8;
	while (cursor <= uri.size()) {
		const std::size_t end = uri.find('&', cursor);
		const std::size_t item_end = end == std::string::npos ? uri.size() : end;
		const std::string item = uri.substr(cursor, item_end - cursor);
		const std::size_t equals = item.find('=');
		if (equals != std::string::npos) {
			const std::string key = percent_decode(item.substr(0, equals));
			const std::string value = percent_decode(item.substr(equals + 1));
			if (key == "xt" && value.rfind("urn:btih:", 0) == 0) {
				const std::string raw = decode_btih(value.substr(9));
				if (!result.info_hash.empty() && result.info_hash != raw) {
					throw std::invalid_argument("magnet URI contains conflicting BTIH values");
				}
				result.info_hash = raw;
			} else if (key == "dn" && result.display_name.empty()) {
				result.display_name = value;
			} else if (key == "tr" && !value.empty()) {
				if (result.trackers.size() >= 20 || value.size() > 2048) {
					throw std::invalid_argument("magnet URI tracker list exceeds its limit");
				}
				result.trackers.push_back(value);
			}
		}
		if (end == std::string::npos) break;
		cursor = end + 1;
	}

	if (result.info_hash.size() != 20) {
		throw std::invalid_argument("magnet URI is missing xt=urn:btih:<hash>");
	}
	result.info_hash_hex = to_hex(result.info_hash);
	return result;
}

}  // namespace p2p::network