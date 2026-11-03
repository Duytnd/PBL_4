#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace pbl4::network {

enum class NatProtocol {
	TCP,
	UDP
};

enum class NatDeviceType {
	None,
	UPnP,
	NatPmp
};

struct PortMappingResult {
	bool success = false;
	NatDeviceType device_type = NatDeviceType::None;
	std::uint16_t internal_port = 0;
	std::uint16_t external_port = 0;
	NatProtocol protocol = NatProtocol::TCP;
	std::uint32_t lifetime = 0;
	std::string external_ip;
	std::string message;
};

// Quản lý kỹ thuật vượt NAT (UPnP IGD và NAT-PMP)
class NatTraversal {
public:
	NatTraversal();
	~NatTraversal();

	NatTraversal(const NatTraversal&) = delete;
	NatTraversal& operator=(const NatTraversal&) = delete;

	NatTraversal(NatTraversal&& other) noexcept;
	NatTraversal& operator=(NatTraversal&& other) noexcept;

	// Dò tìm thiết bị Router Gateway (thử UPnP qua SSDP multicast, sau đó thử NAT-PMP qua Gateway UDP)
	bool discover(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

	// Tự động mở cổng (Port Forwarding / Port Mapping) trên Router
	// external_port = 0 có nghĩa là yêu cầu mở cùng port với internal_port
	PortMappingResult map_port(
		std::uint16_t internal_port,
		std::uint16_t external_port = 0,
		NatProtocol protocol = NatProtocol::TCP,
		std::uint32_t lifetime_sec = 3600);

	// Gỡ bỏ ánh xạ cổng khi tắt ứng dụng
	bool unmap_port(std::uint16_t external_port, NatProtocol protocol = NatProtocol::TCP);

	// Thông tin trạng thái
	[[nodiscard]] std::string external_ip() const { return external_ip_; }
	[[nodiscard]] NatDeviceType device_type() const noexcept { return device_type_; }
	[[nodiscard]] bool is_discovered() const noexcept { return device_type_ != NatDeviceType::None; }

	// =========================================================================
	// Các hàm phụ trợ tĩnh (tiện ích phân tích gói tin, phục vụ unit test độc lập)
	// =========================================================================

	// Parse header LOCATION từ phản hồi SSDP M-SEARCH
	static bool parse_ssdp_response(const std::string& response, std::string& location_url);

	// Parse XML mô tả thiết bị UPnP để lấy controlURL và serviceType
	static bool parse_upnp_description(
		const std::string& xml,
		std::string& control_url,
		std::string& service_type);

	// Tạo body SOAP XML cho action AddPortMapping của UPnP
	static std::string build_upnp_soap_add_port(
		const std::string& service_type,
		const std::string& client_ip,
		std::uint16_t internal_port,
		std::uint16_t external_port,
		NatProtocol protocol,
		std::uint32_t lifetime = 3600,
		const std::string& description = "PBL4 BitTorrent");

	// Tạo body SOAP XML cho action DeletePortMapping của UPnP
	static std::string build_upnp_soap_delete_port(
		const std::string& service_type,
		std::uint16_t external_port,
		NatProtocol protocol);

	// Tạo body SOAP XML cho action GetExternalIPAddress của UPnP
	static std::string build_upnp_soap_get_external_ip(const std::string& service_type);

	// Parse địa chỉ IP từ phản hồi SOAP GetExternalIPAddressResponse
	static bool parse_upnp_external_ip_response(const std::string& xml, std::string& ip_out);

	// Tạo gói tin NAT-PMP yêu cầu External IP (Opcode 0)
	static std::vector<std::uint8_t> build_natpmp_ip_request();

	// Parse phản hồi NAT-PMP External IP (Opcode 128)
	static bool parse_natpmp_ip_response(
		const std::uint8_t* data,
		std::size_t len,
		std::string& ip_out);

	// Tạo gói tin NAT-PMP yêu cầu Map Port (Opcode 1 cho UDP, Opcode 2 cho TCP)
	static std::vector<std::uint8_t> build_natpmp_map_request(
		NatProtocol protocol,
		std::uint16_t internal_port,
		std::uint16_t external_port,
		std::uint32_t lifetime);

	// Parse phản hồi NAT-PMP Map Port (Opcode 129 hoặc 130)
	static bool parse_natpmp_map_response(
		const std::uint8_t* data,
		std::size_t len,
		std::uint16_t& external_port_out,
		std::uint32_t& lifetime_out);

private:
	NatDeviceType device_type_{NatDeviceType::None};
	std::string external_ip_;
	std::string local_ip_;

	// UPnP context
	std::string upnp_control_url_;
	std::string upnp_service_type_;
	std::string upnp_host_;
	std::uint16_t upnp_port_{80};

	// NAT-PMP context
	std::string natpmp_gateway_ip_;
	std::uint16_t natpmp_port_{5351};

	bool discover_upnp(std::chrono::milliseconds timeout);
	bool discover_nat_pmp(std::chrono::milliseconds timeout);

	std::string send_http_request(
		const std::string& host,
		std::uint16_t port,
		const std::string& method,
		const std::string& path,
		const std::string& soap_action,
		const std::string& body,
		std::chrono::milliseconds timeout);
};

}  // namespace pbl4::network

