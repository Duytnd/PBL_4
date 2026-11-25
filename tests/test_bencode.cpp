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

void testDecodePrefixReturnsTrailingPayloadOffset() {
    const std::string rawTail("\0\xff" "DATA", 6);
    const std::string input = "d8:msg_typei1e5:piecei0ee" + rawTail;
    std::size_t next = 0;
    const auto decoded = decodeBencode(input, &next);
    const auto* dict = decoded.get_if<BencodeDict>();
    CHECK(dict != nullptr);
    CHECK_EQ(next, std::size_t(25));
    CHECK_EQ(input.substr(next), rawTail);
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

void testIntegerStrictFormat() {
    // Chỉ chấp nhận -?[1-9][0-9]* hoặc 0.
    CHECK(decodeBencode("i0e") == BencodeValue(int64_t(0)));
    CHECK(decodeBencode("i-5e") == BencodeValue(int64_t(-5)));
    CHECK(decodeBencode("i123e") == BencodeValue(int64_t(123)));
    const char* bad[] = {"i+5e", "i 5e", "i5 e", "i12abce", "i-012e", "i-0e",
                         "i042e", "ie", "i-e", "i--5e", "i5.0e", "i0x10e"};
    for (const char* text : bad) {
        bool threw = false;
        try {
            decodeBencode(text);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "  not rejected: " << text << std::endl;
        }
        CHECK(threw);
    }
    // Vượt int64 vẫn là lỗi (out of range).
    CHECK_THROWS(decodeBencode("i99999999999999999999e"));
}

std::string nestedLists(std::size_t depth) {
    return std::string(depth, 'l') + std::string(depth, 'e');
}

void testNestingDepthLimit() {
    CHECK(decodeBencode(nestedLists(63)).get_if<BencodeList>() != nullptr);
    CHECK(decodeBencode(nestedLists(64)).get_if<BencodeList>() != nullptr);
    const std::size_t bad[] = {65, 10000, 200000};
    for (std::size_t depth : bad) {
        bool threw = false;
        try {
            decodeBencode(nestedLists(depth));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    // Chỉ có phần mở, không đóng, cũng không được crash.
    CHECK_THROWS(decodeBencode(std::string(200000, 'l')));
    // Dict lồng nhau: d1:ad1:a...
    std::string nestedDict;
    for (int i = 0; i < 100000; ++i) nestedDict += "d1:a";
    CHECK_THROWS(decodeBencode(nestedDict));
}

void testStringLengthOverflow() {
    CHECK_THROWS(decodeBencode("4:ab"));
    CHECK_THROWS(decodeBencode("99999999999999999999:x"));
    CHECK_THROWS(decodeBencode("18446744073709551615:x"));
    CHECK_THROWS(decodeBencode("18446744073709551595:x"));
    // Phải kết thúc nhanh và ném invalid_argument (trên code cũ: treo).
    bool threw = false;
    try {
        decodeBencode("l18446744073709551595:x");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    RUN(testEncodeDecodeInteger);
    RUN(testEncodeDecodeString);
    RUN(testEncodeDecodeStringPreservesRawBytes);
    RUN(testEncodeDecodeList);
    RUN(testEncodeDecodeDictSortsKeys);
    RUN(testDecodeRejectsTrailingBytes);
    RUN(testDecodePrefixReturnsTrailingPayloadOffset);
    RUN(testDecodeRejectsMalformedInput);
    RUN(testRoundTripNestedStructure);
    RUN(testIntegerStrictFormat);
    RUN(testNestingDepthLimit);
    // Cuối cùng: trên code cũ ca này treo.
    RUN(testStringLengthOverflow);
    return TEST_REPORT();
}
