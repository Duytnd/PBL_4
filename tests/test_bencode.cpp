#include "test_framework.h"

#include "core/bencode.h"

using namespace p2p;

void testEncodeDecodeInteger() {
    CHECK_EQ(encodeBencode(int64_t(42)), std::string("i42e"));
    CHECK_EQ(encodeBencode(int64_t(-10)), std::string("i-10e"));
    CHECK_EQ(encodeBencode(int64_t(0)), std::string("i0e"));

    CHECK(decodeBencode("i42e") == BencodeValue(int64_t(42)));
    CHECK(decodeBencode("i-10e") == BencodeValue(int64_t(-10)));
}

void testEncodeDecodeString() {
    CHECK_EQ(encodeBencode(std::string("hello")), std::string("5:hello"));
    CHECK_EQ(encodeBencode(std::string("")), std::string("0:"));

    CHECK(decodeBencode("5:hello") == BencodeValue(std::string("hello")));
}

void testEncodeDecodeStringPreservesRawBytes() {
    // info_hash / peer_id là byte nhị phân (có thể chứa 0x00), bencode string
    // phải giữ nguyên, không coi là chuỗi kết thúc bằng NUL (mục 5.2).
    const std::string raw("\x00\x01\xff\x02", 4);
    const std::string encoded = encodeBencode(raw);
    const auto decoded = decodeBencode(encoded);
    const auto* asString = decoded.get_if<std::string>();
    CHECK(asString != nullptr);
    CHECK(*asString == raw);
}

void testEncodeDecodeList() {
    BencodeList list;
    list.emplace_back(std::string("spam"));
    list.emplace_back(std::string("eggs"));
    CHECK_EQ(encodeBencode(list), std::string("l4:spam4:eggse"));

    const auto decoded = decodeBencode("l4:spam4:eggse");
    const auto* asList = decoded.get_if<BencodeList>();
    CHECK(asList != nullptr);
    CHECK_EQ(asList->size(), std::size_t(2));
}

void testEncodeDecodeDictSortsKeys() {
    // Khoá dict phải sắp xếp theo thứ tự byte tăng dần khi ghi ra (mục 5.1).
    BencodeDict dict;
    dict["foo"] = BencodeValue(int64_t(10));
    dict["bar"] = BencodeValue(std::string("hello"));
    // std::map<std::string, ...> đã tự sắp theo thứ tự byte, nên encode phải
    // ra "bar" trước "foo".
    CHECK_EQ(encodeBencode(dict), std::string("d3:bar5:hello3:fooi10ee"));
}

void testDecodeRejectsTrailingBytes() {
    CHECK_THROWS(decodeBencode("i42eGARBAGE"));
}

void testDecodeRejectsMalformedInput() {
    CHECK_THROWS(decodeBencode(""));
    CHECK_THROWS(decodeBencode("i42"));          // thiếu 'e'
    CHECK_THROWS(decodeBencode("5:ab"));          // chuỗi bị cắt ngắn (thiếu byte)
    CHECK_THROWS(decodeBencode("l4:spam"));       // list chưa đóng
    CHECK_THROWS(decodeBencode("d3:fooe"));       // dict thiếu value cho key
    CHECK_THROWS(decodeBencode("di1e5:valuee")); // key không phải string
    CHECK_THROWS(decodeBencode("i012e"));         // số có 0 ở đầu không hợp lệ
}

void testRoundTripNestedStructure() {
    // Mô phỏng 1 phần nhỏ của .torrent: dict chứa list chứa dict con.
    BencodeDict fileEntry;
    fileEntry["length"] = BencodeValue(int64_t(123));
    BencodeList pathList;
    pathList.emplace_back(std::string("sub"));
    pathList.emplace_back(std::string("file.txt"));
    fileEntry["path"] = BencodeValue(pathList);

    BencodeList files;
    files.emplace_back(BencodeValue(fileEntry));

    BencodeDict info;
    info["files"] = BencodeValue(files);
    info["name"] = BencodeValue(std::string("root"));

    const std::string encoded = encodeBencode(info);
    const auto decoded = decodeBencode(encoded);
    CHECK(decoded == BencodeValue(info));

    // Encode lại lần 2 phải cho đúng cùng chuỗi byte (quan trọng vì info_hash
    // phụ thuộc vào việc encode lại cho ra đúng byte gốc).
    CHECK_EQ(encodeBencode(decoded), encoded);
}

int main() {
    RUN(testEncodeDecodeInteger);
    RUN(testEncodeDecodeString);
    RUN(testEncodeDecodeStringPreservesRawBytes);
    RUN(testEncodeDecodeList);
    RUN(testEncodeDecodeDictSortsKeys);
    RUN(testDecodeRejectsTrailingBytes);
    RUN(testDecodeRejectsMalformedInput);
    RUN(testRoundTripNestedStructure);
    return TEST_REPORT();
}
