#include "network/nat_traversal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

#include "network/lan_address.h"

#include <asio.hpp>
#include <functional>
#include <memory>

namespace pbl4::network {

namespace {

std::string to_lower_str(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

// Tìm thẻ XML đơn giản: <tag>value</tag>
bool find_xml_tag(const std::string& xml, const std::string& tag, std::string& value) {
	const std::string open_tag = "<" + tag + ">";
	const std::string close_tag = "</" + tag + ">";

	auto pos1 = xml.find(open_tag);
	if (pos1 == std::string::npos) {
		// Thử tìm thẻ có thuộc tính: <tag ...>
		const std::string prefix = "<" + tag + " ";
		pos1 = xml.find(prefix);
		if (pos1 != std::string::npos) {
			pos1 = xml.find('>', pos1);
			if (pos1 != std::string::npos) {
				pos1 += 1;
			}
		}
	} else {
		pos1 += open_tag.size();
	}

	if (pos1 == std::string::npos) {
		return false;
	}

	const auto pos2 = xml.find(close_tag, pos1);
	if (pos2 == std::string::npos) {
		return false;
	}

	value = xml.substr(pos1, pos2 - pos1);
	// Trim khoảng trắng
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
		value.erase(value.begin());
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
		value.pop_back();
	}
	return true;
}

// Tách URL http://host:port/path
bool parse_http_url(const std::string& url, std::string& host, std::uint16_t& port, std::string& path) {
	const std::string prefix = "http://";
	if (url.rfind(prefix, 0) != 0) {
		return false;
	}

	const std::size_t host_start = prefix.size();
	const std::size_t slash_pos = url.find('/', host_start);

	std::string host_port_part;
	if (slash_pos == std::string::npos) {
		host_port_part = url.substr(host_start);
		path = "/";
	} else {
		host_port_part = url.substr(host_start, slash_pos - host_start);
		path = url.substr(slash_pos);
	}

	const std::size_t colon_pos = host_port_part.find(':');
	if (colon_pos == std::string::npos) {
		host = host_port_part;
		port = 80;
	} else {
		host = host_port_part.substr(0, colon_pos);
		const std::string port_text = host_port_part.substr(colon_pos + 1);
		if (port_text.empty() || port_text.size() > 5 ||
			port_text.find_first_not_of("0123456789") != std::string::npos) {
			return false;
		}
		const unsigned long parsed_port = std::stoul(port_text);
		if (parsed_port == 0 || parsed_port > 65535) {
			return false;  // trước đây bị cắt ngầm về 16 bit hoặc đổi thành 80
		}
		port = static_cast<std::uint16_t>(parsed_port);
	}
	return !host.empty();
}


// Đã nhận đủ header và (nếu có Content-Length) đủ body chưa.
bool http_response_complete(const std::string& response) {
	const auto header_end = response.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		return false;
	}
	const std::string headers = to_lower_str(response.substr(0, header_end));
	const auto pos = headers.find("content-length:");
	if (pos == std::string::npos) {
		return false;  // không biết độ dài: đọc tới khi đóng kết nối hoặc hết hạn
	}
	std::size_t value_start = pos + 15;
	while (value_start < headers.size() && (headers[value_start] == ' ' || headers[value_start] == '\t')) {
		++value_start;
	}
	const std::size_t value_end = headers.find_first_not_of("0123456789", value_start);
	const std::string number = headers.substr(
		value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
	if (number.empty() || number.size() > 9) {
		return false;
	}
	return response.size() - (header_end + 4) >= std::stoul(number);
}

std::string ssdp_search_request(const char* search_target) {
	return std::string("M-SEARCH * HTTP/1.1\r\n"
					   "HOST: 239.255.255.250:1900\r\n"
					   "MAN: \"ssdp:discover\"\r\n"
					   "MX: 1\r\n"
					   "ST: ") + search_target + "\r\n\r\n";
}

}  // namespace

NatTraversal::NatTraversal() = default;
NatTraversal::~NatTraversal() = default;

NatTraversal::NatTraversal(NatTraversal&& other) noexcept = default;
NatTraversal& NatTraversal::operator=(NatTraversal&& other) noexcept = default;

// =========================================================================
// Các hàm phân tích & tạo gói tin tĩnh (Unit testable)
// =========================================================================

bool NatTraversal::parse_ssdp_response(const std::string& response, std::string& location_url) {
	std::istringstream stream(response);
	std::string line;
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const std::string lower = to_lower_str(line);
		if (lower.rfind("location:", 0) == 0) {
			std::size_t pos = 9;
			while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
				pos++;
			}
			location_url = line.substr(pos);
			return !location_url.empty();
		}
	}
	return false;
}

bool NatTraversal::validate_ssdp_location(
	const std::string& location_url,
	const std::string& sender_ip,
	std::string& host,
	std::uint16_t& port,
	std::string& path) {
	asio::error_code ec;
	const auto sender = asio::ip::make_address_v4(sender_ip, ec);
	if (ec || !p2p::network::is_local_network_address(sender_ip)) {
		return false;
	}
	std::string parsed_host, parsed_path;
	std::uint16_t parsed_port = 80;
	if (!parse_http_url(location_url, parsed_host, parsed_port, parsed_path)) {
		return false;
	}
	const auto location_ip = asio::ip::make_address_v4(parsed_host, ec);
	if (ec || location_ip != sender) {
		return false;  // tên miền hoặc IP khác người gửi: có thể là SSRF
	}
	for (unsigned char c : parsed_path) {
		if (c <= 0x20 || c == 0x7F) {
			return false;
		}
	}
	host = parsed_host;
	port = parsed_port;
	path = parsed_path;
	return true;
}

bool NatTraversal::parse_upnp_description(
	const std::string& xml,
	std::string& control_url,
	std::string& service_type) {
	// Tìm các dịch vụ WANIPConnection hoặc WANPPPConnection
	static const std::vector<std::string> target_services = {
		"urn:schemas-upnp-org:service:WANIPConnection:1",
		"urn:schemas-upnp-org:service:WANIPConnection:2",
		"urn:schemas-upnp-org:service:WANPPPConnection:1"
	};

	for (const auto& target : target_services) {
		const auto pos = xml.find(target);
		if (pos == std::string::npos) {
			continue;
		}

		// Tìm khối <service> chứa target
		const auto service_start = xml.rfind("<service>", pos);
		const auto service_end = xml.find("</service>", pos);
		if (service_start == std::string::npos || service_end == std::string::npos) {
			continue;
		}

		const std::string service_block = xml.substr(service_start, service_end - service_start);
		std::string ctrl;
		if (find_xml_tag(service_block, "controlURL", ctrl)) {
			control_url = ctrl;
			service_type = target;
			return true;
		}
	}
	return false;
}

std::string NatTraversal::build_upnp_soap_add_port(
	const std::string& service_type,
	const std::string& client_ip,
	std::uint16_t internal_port,
	std::uint16_t external_port,
	NatProtocol protocol,
	std::uint32_t lifetime,
	const std::string& description) {
	std::ostringstream ss;
	const std::string proto_str = (protocol == NatProtocol::UDP) ? "UDP" : "TCP";

	ss << "<?xml version=\"1.0\"?>\r\n"
	   << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	   << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
	   << "  <s:Body>\r\n"
	   << "    <u:AddPortMapping xmlns:u=\"" << service_type << "\">\r\n"
	   << "      <NewRemoteHost></NewRemoteHost>\r\n"
	   << "      <NewExternalPort>" << external_port << "</NewExternalPort>\r\n"
	   << "      <NewProtocol>" << proto_str << "</NewProtocol>\r\n"
	   << "      <NewInternalPort>" << internal_port << "</NewInternalPort>\r\n"
	   << "      <NewInternalClient>" << client_ip << "</NewInternalClient>\r\n"
	   << "      <NewEnabled>1</NewEnabled>\r\n"
	   << "      <NewPortMappingDescription>" << description << "</NewPortMappingDescription>\r\n"
	   << "      <NewLeaseDuration>" << lifetime << "</NewLeaseDuration>\r\n"
	   << "    </u:AddPortMapping>\r\n"
	   << "  </s:Body>\r\n"
	   << "</s:Envelope>\r\n";
	return ss.str();
}

std::string NatTraversal::build_upnp_soap_delete_port(
	const std::string& service_type,
	std::uint16_t external_port,
	NatProtocol protocol) {
	std::ostringstream ss;
	const std::string proto_str = (protocol == NatProtocol::UDP) ? "UDP" : "TCP";

	ss << "<?xml version=\"1.0\"?>\r\n"
	   << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	   << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
	   << "  <s:Body>\r\n"
	   << "    <u:DeletePortMapping xmlns:u=\"" << service_type << "\">\r\n"
	   << "      <NewRemoteHost></NewRemoteHost>\r\n"
	   << "      <NewExternalPort>" << external_port << "</NewExternalPort>\r\n"
	   << "      <NewProtocol>" << proto_str << "</NewProtocol>\r\n"
	   << "    </u:DeletePortMapping>\r\n"
	   << "  </s:Body>\r\n"
	   << "</s:Envelope>\r\n";
	return ss.str();
}

std::string NatTraversal::build_upnp_soap_get_external_ip(const std::string& service_type) {
	std::ostringstream ss;
	ss << "<?xml version=\"1.0\"?>\r\n"
	   << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	   << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
	   << "  <s:Body>\r\n"
	   << "    <u:GetExternalIPAddress xmlns:u=\"" << service_type << "\">\r\n"
	   << "    </u:GetExternalIPAddress>\r\n"
	   << "  </s:Body>\r\n"
	   << "</s:Envelope>\r\n";
	return ss.str();
}

bool NatTraversal::parse_upnp_external_ip_response(const std::string& xml, std::string& ip_out) {
	return find_xml_tag(xml, "NewExternalIPAddress", ip_out) && !ip_out.empty();
}

std::vector<std::uint8_t> NatTraversal::build_natpmp_ip_request() {
	return {0x00, 0x00};  // Version 0, Opcode 0
}

bool NatTraversal::parse_natpmp_ip_response(
	const std::uint8_t* data,
	std::size_t len,
	std::string& ip_out) {
	if (!data || len < 12) {
		return false;
	}
	if (data[0] != 0x00 || data[1] != 128) {  // Version 0, Opcode 128
		return false;
	}
	const std::uint16_t result_code = (static_cast<std::uint16_t>(data[2]) << 8) | data[3];
	if (result_code != 0) {
		return false;
	}

	ip_out = std::to_string(data[8]) + "." +
			 std::to_string(data[9]) + "." +
			 std::to_string(data[10]) + "." +
			 std::to_string(data[11]);
	return true;
}

std::vector<std::uint8_t> NatTraversal::build_natpmp_map_request(
	NatProtocol protocol,
	std::uint16_t internal_port,
	std::uint16_t external_port,
	std::uint32_t lifetime) {
	std::vector<std::uint8_t> packet(12, 0);
	packet[0] = 0x00;  // Version 0
	packet[1] = (protocol == NatProtocol::UDP) ? 1 : 2;  // Opcode 1=UDP, 2=TCP
	packet[2] = 0x00;  // Reserved
	packet[3] = 0x00;

	packet[4] = static_cast<std::uint8_t>((internal_port >> 8) & 0xFF);
	packet[5] = static_cast<std::uint8_t>(internal_port & 0xFF);

	packet[6] = static_cast<std::uint8_t>((external_port >> 8) & 0xFF);
	packet[7] = static_cast<std::uint8_t>(external_port & 0xFF);

	packet[8] = static_cast<std::uint8_t>((lifetime >> 24) & 0xFF);
	packet[9] = static_cast<std::uint8_t>((lifetime >> 16) & 0xFF);
	packet[10] = static_cast<std::uint8_t>((lifetime >> 8) & 0xFF);
	packet[11] = static_cast<std::uint8_t>(lifetime & 0xFF);
	return packet;
}

bool NatTraversal::parse_natpmp_map_response(
	const std::uint8_t* data,
	std::size_t len,
	std::uint16_t& external_port_out,
	std::uint32_t& lifetime_out) {
	if (!data || len < 16) {
		return false;
	}
	if (data[0] != 0x00 || (data[1] != 129 && data[1] != 130)) {
		return false;
	}
	const std::uint16_t result_code = (static_cast<std::uint16_t>(data[2]) << 8) | data[3];
	if (result_code != 0) {
		return false;
	}

	external_port_out = (static_cast<std::uint16_t>(data[10]) << 8) | data[11];
	lifetime_out = (static_cast<std::uint32_t>(data[12]) << 24) |
				   (static_cast<std::uint32_t>(data[13]) << 16) |
				   (static_cast<std::uint32_t>(data[14]) << 8) |
				   static_cast<std::uint32_t>(data[15]);
	return true;
}

bool NatTraversal::parse_natpmp_map_response(
	const std::uint8_t* data,
	std::size_t len,
	NatProtocol expected_protocol,
	std::uint16_t expected_internal_port,
	std::uint16_t& external_port_out,
	std::uint32_t& lifetime_out,
	bool allow_zero_external_port) {
	if (!data || len < 16) {
		return false;
	}
	const std::uint8_t expected_opcode = (expected_protocol == NatProtocol::UDP) ? 129 : 130;
	if (data[0] != 0x00 || data[1] != expected_opcode) {
		return false;
	}
	const std::uint16_t internal_echo = (static_cast<std::uint16_t>(data[8]) << 8) | data[9];
	if (internal_echo != expected_internal_port) {
		return false;
	}
	std::uint16_t external_port = 0;
	std::uint32_t lifetime = 0;
	if (!parse_natpmp_map_response(data, len, external_port, lifetime) ||
		(external_port == 0 && !allow_zero_external_port)) {
		return false;
	}
	external_port_out = external_port;
	lifetime_out = lifetime;
	return true;
}

void NatTraversal::configure_natpmp_gateway(const std::string& ip, std::uint16_t port) {
	natpmp_gateway_override_ = ip;
	natpmp_port_ = port == 0 ? 5351 : port;
}

// =========================================================================
// Network I/O (UPnP & NAT-PMP)
// =========================================================================

std::string NatTraversal::send_http_request(
	const std::string& host,
	std::uint16_t port,
	const std::string& method,
	const std::string& path,
	const std::string& soap_action,
	const std::string& body,
	std::chrono::milliseconds timeout) {
	try {
		// Chỉ IPv4 literal: không phân giải DNS (resolver đồng bộ không có timeout và mở đường cho DNS rebinding).
		asio::error_code address_ec;
		const auto address = asio::ip::make_address_v4(host, address_ec);
		if (address_ec || port == 0) {
			return {};
		}

		std::ostringstream req;
		req << method << " " << path << " HTTP/1.1\r\n"
			<< "Host: " << host << ":" << port << "\r\n"
			<< "User-Agent: PBL4-BitTorrent/1.0\r\n"
			<< "Connection: close\r\n";
		if (!soap_action.empty()) {
			req << "SOAPAction: \"" << soap_action << "\"\r\n"
				<< "Content-Type: text/xml; charset=\"utf-8\"\r\n";
		}
		if (!body.empty()) {
			req << "Content-Length: " << body.size() << "\r\n\r\n" << body;
		} else {
			req << "\r\n";
		}
		const std::string request_text = req.str();

		asio::io_context ioc;
		asio::ip::tcp::socket socket(ioc);
		asio::steady_timer deadline(ioc);
		deadline.expires_after(timeout);

		bool timed_out = false;
		bool too_large = false;
		std::string response;
		std::array<char, 4096> buffer{};

		// Một đồng hồ duy nhất cho cả kết nối, gửi và nhận: thiết bị im lặng hay nhỏ giọt từng byte cũng
		// không giữ luồng quá `timeout` (trước đây vòng đọc không có hạn, treo cả session.stop()).
		deadline.async_wait([&](const asio::error_code& ec) {
			if (!ec) {
				timed_out = true;
				asio::error_code ignored;
				socket.close(ignored);
			}
		});

		std::function<void()> read_more;
		read_more = [&]() {
			socket.async_read_some(asio::buffer(buffer), [&](const asio::error_code& ec, std::size_t n) {
				if (n > 0) {
					response.append(buffer.data(), n);
					if (response.size() > kMaxHttpResponseBytes) {
						too_large = true;
						asio::error_code ignored;
						socket.close(ignored);
						deadline.cancel();
						return;
					}
					if (http_response_complete(response)) {
						asio::error_code ignored;
						socket.close(ignored);
						deadline.cancel();
						return;
					}
				}
				if (ec) {
					deadline.cancel();  // EOF hoặc lỗi: kết thúc
					return;
				}
				read_more();
			});
		};

		socket.async_connect(asio::ip::tcp::endpoint(address, port), [&](const asio::error_code& ec) {
			if (ec) {
				deadline.cancel();
				return;
			}
			asio::async_write(socket, asio::buffer(request_text), [&](const asio::error_code& write_ec, std::size_t) {
				if (write_ec) {
					deadline.cancel();
					return;
				}
				read_more();
			});
		});

		ioc.run();

		if (too_large) {
			return {};
		}
		if (timed_out && response.find("\r\n\r\n") == std::string::npos) {
			return {};  // quá hạn khi chưa có nổi header: bỏ
		}
		return response;
	} catch (...) {
		return {};
	}
}

bool NatTraversal::discover_upnp(std::chrono::milliseconds timeout) {
	try {
		// Router có thể chờ ngẫu nhiên tới MX giây rồi mới trả lời: không để cửa sổ chờ quá ngắn.
		const auto window = std::max(timeout, std::chrono::milliseconds(1500));

		// Gửi M-SEARCH trên TỪNG card mạng. Một socket không chọn card sẽ dùng card mặc định của hệ điều hành,
		// với máy nhiều card (Tailscale, VPN, máy ảo) thì gói không tới LAN thật và client mãi báo "không có router".
		std::vector<std::string> interfaces = p2p::network::local_ipv4_addresses();
		if (interfaces.empty()) {
			interfaces.push_back(p2p::network::detect_lan_ip());
		}

		asio::io_context ioc;
		struct Probe {
			explicit Probe(asio::io_context& context) : socket(context) {}
			asio::ip::udp::socket socket;
			std::string local_ip;
			std::array<char, 4096> buffer{};
			asio::ip::udp::endpoint sender;
			int errors = 0;
		};
		std::vector<std::unique_ptr<Probe>> probes;
		const asio::ip::udp::endpoint multicast_ep(asio::ip::make_address("239.255.255.250"), 1900);
		const std::string requests[] = {
			ssdp_search_request("urn:schemas-upnp-org:device:InternetGatewayDevice:1"),
			ssdp_search_request("urn:schemas-upnp-org:device:InternetGatewayDevice:2")};

		for (const auto& ip : interfaces) {
			asio::error_code ec;
			const auto local_address = asio::ip::make_address_v4(ip, ec);
			if (ec) continue;
			auto probe = std::make_unique<Probe>(ioc);
			probe->local_ip = ip;
			probe->socket.open(asio::ip::udp::v4(), ec);
			if (ec) continue;
			probe->socket.bind(asio::ip::udp::endpoint(local_address, 0), ec);
			if (ec) continue;
			probe->socket.set_option(asio::ip::multicast::outbound_interface(local_address), ec);
			probe->socket.set_option(asio::ip::multicast::hops(2), ec);
			bool sent = false;
			for (const auto& request : requests) {
				asio::error_code send_ec;
				probe->socket.send_to(asio::buffer(request), multicast_ep, 0, send_ec);
				sent = sent || !send_ec;
			}
			if (sent) {
				probes.push_back(std::move(probe));
			}
		}
		if (probes.empty()) {
			return false;
		}

		asio::steady_timer timer(ioc);
		timer.expires_after(window);
		bool found = false;
		std::string host, path, found_local_ip;
		std::uint16_t port = 80;
		int datagrams = 0;

		auto cancel_all = [&]() {
			for (auto& probe : probes) {
				asio::error_code ignored;
				probe->socket.cancel(ignored);
			}
		};
		timer.async_wait([&](const asio::error_code& ec) {
			if (!ec) cancel_all();
		});

		std::function<void(Probe&)> listen;
		listen = [&](Probe& probe) {
			probe.socket.async_receive_from(
				asio::buffer(probe.buffer), probe.sender,
				[&](const asio::error_code& ec, std::size_t bytes) {
					if (found || ec == asio::error::operation_aborted) {
						return;
					}
					if (!ec && bytes > 0) {
						// Chặn kẻ làm ngập: chỉ xử lý một số gói hữu hạn trong cửa sổ chờ.
						if (++datagrams > 64) {
							timer.cancel();
							cancel_all();
							return;
						}
						const std::string response(probe.buffer.data(), bytes);
						std::string location, candidate_host, candidate_path;
						std::uint16_t candidate_port = 80;
						if (parse_ssdp_response(response, location) &&
							validate_ssdp_location(location, probe.sender.address().to_string(),
												   candidate_host, candidate_port, candidate_path)) {
							found = true;
							host = candidate_host;
							port = candidate_port;
							path = candidate_path;
							found_local_ip = probe.local_ip;
							timer.cancel();
							cancel_all();
							return;
						}
					} else if (++probe.errors > 16) {
						return;  // lỗi lặp lại (vd. ICMP unreachable trên Windows): thôi nghe socket này
					}
					listen(probe);  // gói không hợp lệ hoặc lỗi nhất thời: nghe tiếp tới khi hết hạn
				});
		};
		for (auto& probe : probes) {
			listen(*probe);
		}
		ioc.run();

		if (!found) {
			return false;
		}
		local_ip_ = found_local_ip;

		// Đọc XML mô tả thiết bị (host đã được xác nhận là chính thiết bị đã trả lời).
		const auto http_timeout = std::max(timeout, std::chrono::milliseconds(2000));
		const std::string desc_xml = send_http_request(host, port, "GET", path, "", "", http_timeout);
		if (desc_xml.empty()) {
			return false;
		}

		std::string ctrl_url, s_type;
		if (!parse_upnp_description(desc_xml, ctrl_url, s_type)) {
			return false;
		}

		upnp_host_ = host;
		upnp_port_ = port;
		upnp_service_type_ = s_type;

		// Chuẩn hóa control url (có thể là đường dẫn tương đối). Chỉ giữ phần đường dẫn: mọi yêu cầu SOAP
		// luôn đi tới đúng thiết bị đã xác nhận, không theo host do XML tự khai.
		if (ctrl_url.rfind("http://", 0) == 0) {
			std::string dummy_host;
			std::uint16_t dummy_port;
			if (!parse_http_url(ctrl_url, dummy_host, dummy_port, upnp_control_url_)) {
				return false;
			}
		} else {
			if (ctrl_url.empty() || ctrl_url.front() != '/') {
				ctrl_url = "/" + ctrl_url;
			}
			upnp_control_url_ = ctrl_url;
		}

		device_type_ = NatDeviceType::UPnP;

		// Thử lấy External IP qua SOAP
		const std::string get_ip_body = build_upnp_soap_get_external_ip(upnp_service_type_);
		const std::string soap_action = upnp_service_type_ + "#GetExternalIPAddress";
		const std::string ip_resp = send_http_request(
			upnp_host_, upnp_port_, "POST", upnp_control_url_, soap_action, get_ip_body, http_timeout);

		parse_upnp_external_ip_response(ip_resp, external_ip_);
		return true;
	} catch (...) {
		return false;
	}
}

bool NatTraversal::discover_nat_pmp(std::chrono::milliseconds timeout) {
	try {
		// Gateway = hop kế tiếp thật của route mặc định, không đoán 192.168.1.1/10.0.0.1... Không biết gateway
		// thì bỏ qua (không gửi gói NAT-PMP tới một địa chỉ ngẫu nhiên của mạng lạ).
		const std::string gateway = natpmp_gateway_override_.empty()
			? p2p::network::default_gateway_ipv4()
			: natpmp_gateway_override_;
		if (gateway.empty()) {
			return false;
		}
		asio::error_code address_ec;
		const auto gateway_address = asio::ip::make_address_v4(gateway, address_ec);
		if (address_ec) {
			return false;
		}
		const asio::ip::udp::endpoint gateway_ep(gateway_address, natpmp_port_);

		asio::io_context ioc;
		asio::ip::udp::socket socket(ioc);
		socket.open(asio::ip::udp::v4());
		const auto request = build_natpmp_ip_request();
		socket.send_to(asio::buffer(request), gateway_ep);

		std::array<std::uint8_t, 64> response{};
		asio::ip::udp::endpoint sender;
		asio::steady_timer timer(ioc);
		timer.expires_after(timeout);
		timer.async_wait([&](const asio::error_code& ec) {
			if (!ec) {
				asio::error_code ignored;
				socket.cancel(ignored);
			}
		});

		bool found = false;
		std::string external_ip;
		int ignored_packets = 0;
		std::function<void()> listen;
		listen = [&]() {
			socket.async_receive_from(
				asio::buffer(response), sender,
				[&](const asio::error_code& ec, std::size_t bytes) {
					if (found || ec == asio::error::operation_aborted) {
						return;
					}
					if (!ec && sender == gateway_ep && bytes >= 12 &&
						parse_natpmp_ip_response(response.data(), bytes, external_ip)) {
						found = true;
						timer.cancel();
						return;
					}
					// Gói từ máy khác (giả mạo) hoặc lỗi nhất thời: bỏ qua và chờ tiếp tới khi hết hạn.
					if (++ignored_packets > 32) {
						timer.cancel();
						return;
					}
					listen();
				});
		};
		listen();
		ioc.run();

		if (!found) {
			return false;
		}
		external_ip_ = external_ip;
		natpmp_gateway_ip_ = gateway;
		device_type_ = NatDeviceType::NatPmp;
		return true;
	} catch (...) {
		return false;
	}
}

bool NatTraversal::discover(std::chrono::milliseconds timeout) {
	if (discover_upnp(timeout / 2)) {
		return true;
	}
	return discover_nat_pmp(timeout / 2);
}

PortMappingResult NatTraversal::map_port(
	std::uint16_t internal_port,
	std::uint16_t external_port,
	NatProtocol protocol,
	std::uint32_t lifetime_sec) {
	PortMappingResult result;
	result.internal_port = internal_port;
	result.external_port = (external_port == 0) ? internal_port : external_port;
	result.protocol = protocol;
	result.device_type = device_type_;
	result.external_ip = external_ip_;

	if (device_type_ == NatDeviceType::None) {
		result.success = false;
		result.message = "No UPnP or NAT-PMP gateway discovered";
		return result;
	}

	if (device_type_ == NatDeviceType::UPnP) {
		const std::string client_ip = local_ip_.empty() ? p2p::network::detect_lan_ip() : local_ip_;
		const std::string body = build_upnp_soap_add_port(
			upnp_service_type_, client_ip, result.internal_port, result.external_port,
			protocol, lifetime_sec, "PBL4 BitTorrent");
		const std::string action = upnp_service_type_ + "#AddPortMapping";

		const std::string resp = send_http_request(
			upnp_host_, upnp_port_, "POST", upnp_control_url_, action, body,
			std::chrono::milliseconds(2000));

		// HTTP 200 OK báo hiệu thành công
		if (resp.find("200 OK") != std::string::npos ||
			resp.find("AddPortMappingResponse") != std::string::npos) {
			result.success = true;
			result.lifetime = lifetime_sec;
			result.message = "UPnP port mapping succeeded";
		} else {
			result.success = false;
			result.message = "UPnP AddPortMapping failed: " + resp;
		}
		return result;
	}

	if (device_type_ == NatDeviceType::NatPmp) {
		try {
			asio::error_code address_ec;
			const auto gateway_address = asio::ip::make_address_v4(natpmp_gateway_ip_, address_ec);
			if (address_ec) {
				result.success = false;
				result.message = "NAT-PMP gateway address is invalid";
				return result;
			}
			const asio::ip::udp::endpoint gateway_ep(gateway_address, natpmp_port_);

			asio::io_context ioc;
			asio::ip::udp::socket socket(ioc);
			socket.open(asio::ip::udp::v4());
			const auto request = build_natpmp_map_request(
				protocol, result.internal_port, result.external_port, lifetime_sec);
			socket.send_to(asio::buffer(request), gateway_ep);

			std::array<std::uint8_t, 64> response{};
			asio::ip::udp::endpoint sender;
			asio::steady_timer timer(ioc);
			timer.expires_after(std::chrono::milliseconds(2000));
			timer.async_wait([&](const asio::error_code& ec) {
				if (!ec) {
					asio::error_code ignored;
					socket.cancel(ignored);
				}
			});

			bool ok = false;
			int ignored_packets = 0;
			std::function<void()> listen;
			listen = [&]() {
				socket.async_receive_from(
					asio::buffer(response), sender,
					[&](const asio::error_code& ec, std::size_t bytes) {
						if (ok || ec == asio::error::operation_aborted) {
							return;
						}
						std::uint16_t mapped_ext_port = 0;
						std::uint32_t granted_lifetime = 0;
						// Chỉ tin gói từ đúng gateway và đúng là phản hồi cho yêu cầu này (giao thức + cổng nội bộ).
						if (!ec && sender == gateway_ep &&
							parse_natpmp_map_response(response.data(), bytes, protocol, result.internal_port,
													  mapped_ext_port, granted_lifetime, lifetime_sec == 0)) {
							result.external_port = mapped_ext_port;
							result.lifetime = granted_lifetime;
							result.success = true;
							ok = true;
							timer.cancel();
							return;
						}
						if (++ignored_packets > 32) {
							timer.cancel();
							return;
						}
						listen();
					});
			};
			listen();
			ioc.run();

			if (ok) {
				result.message = "NAT-PMP port mapping succeeded";
			} else {
				result.success = false;
				result.message = "NAT-PMP mapping timed out or rejected";
			}
			return result;
		} catch (const std::exception& e) {
			result.success = false;
			result.message = e.what();
			return result;
		}
	}

	return result;
}

bool NatTraversal::unmap_port(std::uint16_t external_port, NatProtocol protocol) {
	if (device_type_ == NatDeviceType::UPnP) {
		const std::string body = build_upnp_soap_delete_port(upnp_service_type_, external_port, protocol);
		const std::string action = upnp_service_type_ + "#DeletePortMapping";
		const std::string resp = send_http_request(
			upnp_host_, upnp_port_, "POST", upnp_control_url_, action, body,
			std::chrono::milliseconds(1500));
		return (resp.find("200 OK") != std::string::npos ||
				resp.find("DeletePortMappingResponse") != std::string::npos);
	}

	if (device_type_ == NatDeviceType::NatPmp) {
		// Trong NAT-PMP, unmap là gửi request với lifetime = 0
		const auto res = map_port(external_port, external_port, protocol, 0);
		return res.success;
	}

	return false;
}

}  // namespace pbl4::network

