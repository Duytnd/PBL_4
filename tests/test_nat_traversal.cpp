#include "network/nat_traversal.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace pbl4::network;

void test_ssdp_response_parsing() {
	std::cout << "[TEST] SSDP response parsing... ";
	const std::string ssdp_raw =
		"HTTP/1.1 200 OK\r\n"
		"CACHE-CONTROL: max-age=1800\r\n"
		"DATE: Sun, 01 Nov 2026 12:00:00 GMT\r\n"
		"EXT:\r\n"
		"LOCATION: http://192.168.1.1:1900/rootDesc.xml\r\n"
		"SERVER: Linux/2.6 UPnP/1.0 miniupnpd/1.0\r\n"
		"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
		"USN: uuid:12345678-0000-0000-0000-000000000000::upnp:rootdevice\r\n"
		"\r\n";

	std::string location;
	const bool ok = NatTraversal::parse_ssdp_response(ssdp_raw, location);
	assert(ok);
	assert(location == "http://192.168.1.1:1900/rootDesc.xml");
	std::cout << "PASSED\n";
}

void test_upnp_description_parsing() {
	std::cout << "[TEST] UPnP description XML parsing... ";
	const std::string xml_desc =
		"<?xml version=\"1.0\"?>\n"
		"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
		"  <device>\n"
		"    <serviceList>\n"
		"      <service>\n"
		"        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>\n"
		"        <controlURL>/l3f</controlURL>\n"
		"      </service>\n"
		"      <service>\n"
		"        <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
		"        <controlURL>/ipc</controlURL>\n"
		"        <eventSubURL>/ipc_event</eventSubURL>\n"
		"        <SCPDURL>/gateconnSCPD.xml</SCPDURL>\n"
		"      </service>\n"
		"    </serviceList>\n"
		"  </device>\n"
		"</root>";

	std::string control_url;
	std::string service_type;
	const bool ok = NatTraversal::parse_upnp_description(xml_desc, control_url, service_type);
	assert(ok);
	assert(control_url == "/ipc");
	assert(service_type == "urn:schemas-upnp-org:service:WANIPConnection:1");
	std::cout << "PASSED\n";
}

void test_upnp_soap_builders_and_response() {
	std::cout << "[TEST] UPnP SOAP message generation and response parsing... ";
	const std::string s_type = "urn:schemas-upnp-org:service:WANIPConnection:1";

	const std::string add_soap = NatTraversal::build_upnp_soap_add_port(
		s_type, "192.168.1.50", 6881, 6881, NatProtocol::TCP, 3600, "PBL4 Test");
	assert(add_soap.find("<NewInternalPort>6881</NewInternalPort>") != std::string::npos);
	assert(add_soap.find("<NewProtocol>TCP</NewProtocol>") != std::string::npos);
	assert(add_soap.find("<NewInternalClient>192.168.1.50</NewInternalClient>") != std::string::npos);

	const std::string del_soap = NatTraversal::build_upnp_soap_delete_port(s_type, 6881, NatProtocol::TCP);
	assert(del_soap.find("<NewExternalPort>6881</NewExternalPort>") != std::string::npos);

	const std::string ip_response_xml =
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
		"  <s:Body>\n"
		"    <u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">\n"
		"      <NewExternalIPAddress>203.0.113.195</NewExternalIPAddress>\n"
		"    </u:GetExternalIPAddressResponse>\n"
		"  </s:Body>\n"
		"</s:Envelope>";

	std::string external_ip;
	const bool ok_ip = NatTraversal::parse_upnp_external_ip_response(ip_response_xml, external_ip);
	assert(ok_ip);
	assert(external_ip == "203.0.113.195");
	std::cout << "PASSED\n";
}

void test_natpmp_requests_and_responses() {
	std::cout << "[TEST] NAT-PMP packets roundtrip encoding and decoding... ";

	// 1. IP Request & Response
	const auto ip_req = NatTraversal::build_natpmp_ip_request();
	assert(ip_req.size() == 2);
	assert(ip_req[0] == 0 && ip_req[1] == 0);

	// Giả lập router trả về IP 198.51.100.42 (Result Code 0, Epoch 1000)
	std::vector<std::uint8_t> ip_resp = {
		0x00, 0x80,             // Vers 0, Opcode 128
		0x00, 0x00,             // Result 0
		0x00, 0x00, 0x03, 0xE8, // Epoch 1000
		198, 51, 100, 42        // External IP
	};

	std::string parsed_ip;
	const bool ok_natpmp_ip = NatTraversal::parse_natpmp_ip_response(
		ip_resp.data(), ip_resp.size(), parsed_ip);
	assert(ok_natpmp_ip);
	assert(parsed_ip == "198.51.100.42");

	// 2. Map Port Request & Response (TCP port 6881 -> external 6881, 7200s)
	const auto map_req = NatTraversal::build_natpmp_map_request(
		NatProtocol::TCP, 6881, 6881, 7200);
	assert(map_req.size() == 12);
	assert(map_req[1] == 2);  // Opcode 2 = TCP

	std::vector<std::uint8_t> map_resp = {
		0x00, 130,              // Vers 0, Opcode 130 (128 + 2)
		0x00, 0x00,             // Result 0
		0x00, 0x00, 0x03, 0xE8, // Epoch 1000
		0x1A, 0xE1,             // Internal port 6881
		0x1A, 0xE1,             // External port 6881
		0x00, 0x00, 0x1C, 0x20  // Lifetime 7200s
	};

	std::uint16_t mapped_port = 0;
	std::uint32_t granted_lifetime = 0;
	const bool ok_map = NatTraversal::parse_natpmp_map_response(
		map_resp.data(), map_resp.size(), mapped_port, granted_lifetime);
	assert(ok_map);
	assert(mapped_port == 6881);
	assert(granted_lifetime == 7200);

	std::cout << "PASSED\n";
}

int main() {
	std::cout << "================ NAT TRAVERSAL TESTS ================\n";
	test_ssdp_response_parsing();
	test_upnp_description_parsing();
	test_upnp_soap_builders_and_response();
	test_natpmp_requests_and_responses();
	std::cout << "All NAT Traversal tests PASSED!\n";
	return 0;
}

