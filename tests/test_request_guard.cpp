#include "test_framework.h"

#include "webui/request_guard.h"

using namespace p2p::webui;

void testAllowedHostAcceptsLocalAndPrivateLiterals() {
    CHECK(isAllowedHost("localhost"));
    CHECK(isAllowedHost("localhost:8080"));
    CHECK(isAllowedHost("LOCALHOST:8080"));
    CHECK(isAllowedHost("127.0.0.1:8080"));
    CHECK(isAllowedHost("127.5.6.7"));
    CHECK(isAllowedHost("[::1]:8080"));
    CHECK(isAllowedHost("10.0.0.5:8080"));
    CHECK(isAllowedHost("192.168.1.232:8080"));
    CHECK(isAllowedHost("172.16.0.1"));
    CHECK(isAllowedHost("172.31.255.254:80"));
    CHECK(isAllowedHost("100.71.46.96:8080"));      // Tailscale / CGNAT
    CHECK(isAllowedHost("http://192.168.1.5:8080"));  // dạng Origin
}

void testAllowedHostRejectsDnsRebindingNames() {
    // Các tên miền này từng qua được kiểm tra tiền tố chuỗi.
    CHECK(!isAllowedHost("10.evil.com"));
    CHECK(!isAllowedHost("10.evil.com:8080"));
    CHECK(!isAllowedHost("192.168.evil.com"));
    CHECK(!isAllowedHost("172.evil.com"));
    CHECK(!isAllowedHost("127.0.0.1.evil.com"));
    CHECK(!isAllowedHost("http://10.attacker.net:8080/"));
    CHECK(!isAllowedHost("evil.com"));
    CHECK(!isAllowedHost("evil.com:8080"));
    CHECK(!isAllowedHost("localhost.evil.com"));
    CHECK(!isAllowedHost("user@evil.com"));
    CHECK(!isAllowedHost("127.0.0.1@evil.com"));  // userinfo che địa chỉ thật
}

void testAllowedHostRejectsPublicAndMalformedAddresses() {
    CHECK(!isAllowedHost("172.217.1.1"));    // IP public của Google, không thuộc 172.16/12
    CHECK(!isAllowedHost("172.15.0.1"));
    CHECK(!isAllowedHost("172.32.0.1"));
    CHECK(!isAllowedHost("8.8.8.8"));
    CHECK(!isAllowedHost("100.63.255.255"));
    CHECK(!isAllowedHost("100.128.0.1"));
    CHECK(!isAllowedHost("192.169.1.1"));
    CHECK(!isAllowedHost("11.0.0.1"));
    CHECK(!isAllowedHost("0.0.0.0"));
    CHECK(!isAllowedHost("256.1.1.1"));
    CHECK(!isAllowedHost("10.0.0"));
    CHECK(!isAllowedHost("10.0.0.1.5"));
    CHECK(!isAllowedHost("010.0.0.1"));      // số 0 đứng đầu mơ hồ (bát phân)
    CHECK(!isAllowedHost("[::2]:80"));
    CHECK(!isAllowedHost("[2001:db8::1]"));
    CHECK(!isAllowedHost(""));
    CHECK(!isAllowedHost(":8080"));
}

void testSameOriginRequiresExactHost() {
    CHECK(isSameOrigin("", "localhost:8080"));  // curl / công cụ không gửi Origin
    CHECK(isSameOrigin("http://localhost:8080", "localhost:8080"));
    CHECK(isSameOrigin("http://192.168.1.232:8080", "192.168.1.232:8080"));
    CHECK(isSameOrigin("HTTP://LocalHost:8080", "localhost:8080"));
    // Trang web của máy khác trong LAN: cũng là IP nội bộ nhưng không phải chính WebUI.
    CHECK(!isSameOrigin("http://192.168.1.50", "192.168.1.232:8080"));
    CHECK(!isSameOrigin("http://192.168.1.232:9000", "192.168.1.232:8080"));
    CHECK(!isSameOrigin("http://evil.com", "localhost:8080"));
    // Host và Origin cùng là tên miền kẻ tấn công: isSameOrigin đúng, nhưng lớp isAllowedHost phải chặn ngay từ Host.
    CHECK(isSameOrigin("http://10.evil.com", "10.evil.com"));
    CHECK(!isAllowedHost("10.evil.com"));
    CHECK(!isSameOrigin("null", "localhost:8080"));
    CHECK(!isSameOrigin("https://localhost:8080", "localhost:8080"));
    CHECK(!isSameOrigin("http://localhost:8080", ""));
}

void testOpenableFileNameAllowsOnlyDocumentLikeTypes() {
    CHECK(isOpenableFileName("photo.jpg"));
    CHECK(isOpenableFileName("movie.MKV"));
    CHECK(isOpenableFileName("report.pdf"));
    CHECK(isOpenableFileName("notes.txt"));
    CHECK(isOpenableFileName("archive.zip"));
    CHECK(isOpenableFileName("tên tiếng Việt.docx"));
    CHECK(isOpenableFileName("a b (1).png"));
}

void testOpenableFileNameBlocksExecutablesAndDisguises() {
    for (const char* bad : {"setup.exe", "run.bat", "run.cmd", "x.com", "x.scr", "x.msi", "x.ps1", "x.vbs", "x.js",
                            "x.jar", "x.lnk", "x.url", "x.hta", "x.reg", "x.dll", "x.sh", "x.py", "x.app",
                            "x.svg", "x.docm", "x.xlsm", "x.html", "x.htm", "x.pif", "x.cpl", "x.msc"}) {
        CHECK(!isOpenableFileName(bad));
    }
    CHECK(!isOpenableFileName("photo.jpg.exe"));     // đuôi kép: đuôi cuối mới quyết định
    CHECK(!isOpenableFileName("evil.exe."));         // Windows bỏ dấu chấm cuối
    CHECK(!isOpenableFileName("evil.exe "));         // và khoảng trắng cuối
    CHECK(!isOpenableFileName("photo.jpg:evil.exe")); // NTFS alternate data stream
    CHECK(!isOpenableFileName("noextension"));
    CHECK(!isOpenableFileName("trailingdot."));
    CHECK(!isOpenableFileName(""));
}

void testOpenableFileNameBlocksShellMetacharacters() {
    for (const char* bad : {"a\"b.txt", "a$(id).txt", "a`id`.txt", "a;b.txt", "a&b.txt", "a|b.txt", "a%PATH%.txt",
                            "a>b.txt", "a<b.txt", "a?.txt", "a*.txt", "../a.txt", "dir/a.txt", "dir\\a.txt"}) {
        CHECK(!isOpenableFileName(bad));
    }
    CHECK(!isOpenableFileName(std::string("a\x01.txt")));
    CHECK(!isOpenableFileName(std::string("a\nb.txt")));
    CHECK(!isOpenableFileName(std::string(300, 'a') + ".txt"));
}

void testOpenableDirectoryName() {
    CHECK(isOpenableDirectoryName("mfsel"));
    CHECK(isOpenableDirectoryName("My Torrent 2024"));
    CHECK(!isOpenableDirectoryName(""));
    CHECK(!isOpenableDirectoryName("."));
    CHECK(!isOpenableDirectoryName(".."));
    CHECK(!isOpenableDirectoryName("a$(id)"));
    CHECK(!isOpenableDirectoryName("a\"b"));
    CHECK(!isOpenableDirectoryName("trailing."));
}

int main() {
    RUN(testAllowedHostAcceptsLocalAndPrivateLiterals);
    RUN(testAllowedHostRejectsDnsRebindingNames);
    RUN(testAllowedHostRejectsPublicAndMalformedAddresses);
    RUN(testSameOriginRequiresExactHost);
    RUN(testOpenableFileNameAllowsOnlyDocumentLikeTypes);
    RUN(testOpenableFileNameBlocksExecutablesAndDisguises);
    RUN(testOpenableFileNameBlocksShellMetacharacters);
    RUN(testOpenableDirectoryName);
    return TEST_REPORT();
}
