#include "network/nat_traversal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

#include <asio.hpp>

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
		try {
			port = static_cast<std::uint16_t>(std::stoul(host_port_part.substr(colon_pos + 1)));
		} catch (...) {
			port = 80;
		}
	}
	return !host.empty();
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
		asio::io_context ioc;
		asio::ip::tcp::resolver resolver(ioc);
		auto endpoints = resolver.resolve(host, std::to_string(port));

		asio::ip::tcp::socket socket(ioc);
		asio::steady_timer timer(ioc);
		timer.expires_after(timeout);

		bool timed_out = false;
		timer.async_wait([&](const asio::error_code& ec) {
			if (!ec) {
				timed_out = true;
				socket.cancel();
			}
		});

		asio::async_connect(socket, endpoints, [&](const asio::error_code&, const asio::ip::tcp::endpoint&) {
			timer.cancel();
		});

		ioc.restart();
		ioc.run();
		if (timed_out || !socket.is_open()) {
			return {};
		}

		// Chuẩn bị HTTP request
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

		const std::string req_str = req.str();
		asio::write(socket, asio::buffer(req_str));

		// Đọc toàn bộ phản hồi
		std::string response;
		std::array<char, 2048> buf{};
		asio::error_code ec;
		while (true) {
			const std::size_t n = socket.read_some(asio::buffer(buf), ec);
			if (n > 0) {
				response.append(buf.data(), n);
			}
			if (ec) {
				break;
			}
		}
		return response;
	} catch (...) {
		return {};
	}
}

bool NatTraversal::discover_upnp(std::chrono::milliseconds timeout) {
	try {
		asio::io_context ioc;
		asio::ip::udp::socket socket(ioc);
		socket.open(asio::ip::udp::v4());

		// Cho phép gửi multicast
		socket.set_option(asio::socket_base::broadcast(true));

		const std::string ssdp_request =
			"M-SEARCH * HTTP/1.1\r\n"
			"HOST: 239.255.255.250:1900\r\n"
			"MAN: \"ssdp:discover\"\r\n"
			"MX: 2\r\n"
			"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
			"\r\n";

		const asio::ip::udp::endpoint multicast_ep(
			asio::ip::make_address("239.255.255.250"), 1900);
		socket.send_to(asio::buffer(ssdp_request), multicast_ep);

		// Nhận phản hồi
		std::array<char, 4096> recv_buf{};
		asio::ip::udp::endpoint sender_ep;
		asio::steady_timer timer(ioc);
		timer.expires_after(timeout);

		bool got_response = false;
		std::string ssdp_response;

		timer.async_wait([&](const asio::error_code& ec) {
			if (!ec) {
				socket.cancel();
			}
		});

		socket.async_receive_from(
			asio::buffer(recv_buf), sender_ep,
			[&](const asio::error_code& ec, std::size_t bytes_recvd) {
				if (!ec && bytes_recvd > 0) {
					got_response = true;
					ssdp_response.assign(recv_buf.data(), bytes_recvd);
					timer.cancel();
				}
			});

		ioc.restart();
		ioc.run();

		if (!got_response) {
			return false;
		}

		// Lấy local IP từ socket
		asio::error_code local_ec;
		const auto local_ep = socket.local_endpoint(local_ec);
		if (!local_ec) {
			local_ip_ = local_ep.address().to_string();
		}

		std::string location_url;
		if (!parse_ssdp_response(ssdp_response, location_url)) {
			return false;
		}

		// Đọc XML mô tả thiết bị
		std::string host, path;
		std::uint16_t port = 80;
		if (!parse_http_url(location_url, host, port, path)) {
			return false;
		}

		const std::string desc_xml = send_http_request(host, port, "GET", path, "", "", timeout);
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

		// Chuẩn hóa control url (có thể là đường dẫn tương đối)
		if (ctrl_url.rfind("http://", 0) == 0) {
			std::string dummy_host;
			std::uint16_t dummy_port;
			parse_http_url(ctrl_url, dummy_host, dummy_port, upnp_control_url_);
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
			upnp_host_, upnp_port_, "POST", upnp_control_url_, soap_action, get_ip_body, timeout);

		parse_upnp_external_ip_response(ip_resp, external_ip_);
		return true;
	} catch (...) {
		return false;
	}
}

bool NatTraversal::discover_nat_pmp(std::chrono::milliseconds timeout) {
	try {
		// Thử default gateway IP (thường là 192.168.1.1 hoặc 192.168.0.1)
		static const std::vector<std::string> candidate_gateways = {
			"192.168.1.1", "192.168.0.1", "10.0.0.1"
		};

		asio::io_context ioc;
		for (const auto& gw : candidate_gateways) {
			try {
				asio::ip::udp::socket socket(ioc);
				socket.open(asio::ip::udp::v4());

				const auto req = build_natpmp_ip_request();
				const asio::ip::udp::endpoint gw_ep(asio::ip::make_address(gw), natpmp_port_);
				socket.send_to(asio::buffer(req), gw_ep);

				std::array<std::uint8_t, 64> resp{};
				asio::ip::udp::endpoint sender_ep;
				asio::steady_timer timer(ioc);
				timer.expires_after(timeout);

				bool got_response = false;
				timer.async_wait([&](const asio::error_code& ec) {
					if (!ec) socket.cancel();
				});

				socket.async_receive_from(
					asio::buffer(resp), sender_ep,
					[&](const asio::error_code& ec, std::size_t bytes_recvd) {
						if (!ec && bytes_recvd >= 12) {
							if (parse_natpmp_ip_response(resp.data(), bytes_recvd, external_ip_)) {
								got_response = true;
								timer.cancel();
							}
						}
					});

				ioc.restart();
				ioc.run();

				if (got_response) {
					natpmp_gateway_ip_ = gw;
					device_type_ = NatDeviceType::NatPmp;
					return true;
				}
			} catch (...) {
				continue;
			}
		}
		return false;
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
		const std::string client_ip = local_ip_.empty() ? "192.168.1.100" : local_ip_;
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
			asio::io_context ioc;
			asio::ip::udp::socket socket(ioc);
			socket.open(asio::ip::udp::v4());

			const auto req = build_natpmp_map_request(
				protocol, result.internal_port, result.external_port, lifetime_sec);
			const asio::ip::udp::endpoint gw_ep(
				asio::ip::make_address(natpmp_gateway_ip_), natpmp_port_);
			socket.send_to(asio::buffer(req), gw_ep);

			std::array<std::uint8_t, 64> resp{};
			asio::ip::udp::endpoint sender_ep;
			asio::steady_timer timer(ioc);
			timer.expires_after(std::chrono::milliseconds(2000));

			bool ok = false;
			timer.async_wait([&](const asio::error_code& ec) {
				if (!ec) socket.cancel();
			});

			socket.async_receive_from(
				asio::buffer(resp), sender_ep,
				[&](const asio::error_code& ec, std::size_t bytes_recvd) {
					if (!ec && bytes_recvd >= 16) {
						std::uint16_t mapped_ext_port = 0;
						std::uint32_t granted_lifetime = 0;
						if (parse_natpmp_map_response(resp.data(), bytes_recvd, mapped_ext_port, granted_lifetime)) {
							result.external_port = mapped_ext_port;
							result.lifetime = granted_lifetime;
							result.success = true;
							ok = true;
							timer.cancel();
						}
					}
				});

			ioc.restart();
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

