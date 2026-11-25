#pragma once

#include <string>

namespace p2p::webui {

// Hostname/IP của header Host hoặc Origin ("host", "host:port", "scheme://host:port/...", "[::1]:8080")
// có được phép truy cập WebUI không. Chỉ nhận:
//  - "localhost"
//  - IPv4 literal hợp lệ thuộc loopback 127/8, RFC1918 (10/8, 172.16/12, 192.168/16) hoặc 100.64/10 (Tailscale/CGNAT)
//  - IPv6 loopback "::1"
// Tên miền khác bị từ chối, kể cả khi bắt đầu bằng "10." hay "192.168." (chống DNS rebinding bằng tên miền
// như 10.evil.com), và 172.x ngoài 172.16-31 (vd. 172.217.x.x của Google) cũng không qua.
bool isAllowedHost(const std::string& value);

// Địa chỉ peer của kết nối (req.remote_addr) có phải loopback không: 127.0.0.0/8, "::1", "::ffff:127.x.x.x".
// Chuỗi rỗng hay không phải IP literal hợp lệ đều bị từ chối.
bool isLoopbackAddress(const std::string& address);

// Request đổi trạng thái phải cùng nguồn gốc: header Origin (nếu có) phải đúng "http://<Host>".
// Origin rỗng (công cụ dòng lệnh như curl) được chấp nhận; "null" bị từ chối.
bool isSameOrigin(const std::string& origin, const std::string& hostHeader);

// Tên file có được mở bằng ứng dụng mặc định của hệ điều hành từ WebUI không.
// Chỉ cho các loại không thực thi (ảnh, video, âm thanh, tài liệu, văn bản, nén). Từ chối .exe/.bat/.lnk/...,
// tên có ký tự điều khiển hay ký tự đặc biệt của shell/NTFS (": < > " | ? * ` $ ; & %),
// và đuôi file bị che bằng dấu chấm/khoảng trắng cuối ("a.exe." chạy như "a.exe" trên Windows).
bool isOpenableFileName(const std::string& fileName);

// Tên thư mục (vd. thư mục gốc của torrent nhiều file) có được mở không: chỉ cần tên không chứa ký tự nguy hiểm.
bool isOpenableDirectoryName(const std::string& dirName);

}  // namespace p2p::webui
