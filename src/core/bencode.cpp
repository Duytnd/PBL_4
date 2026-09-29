#include "core/bencode.h"

#include <stdexcept>
#include <string_view>

namespace p2p {
namespace {

std::string encodeStringValue(const std::string& value) {
    // Ví dụ: "hello" -> "5:hello"
    // nghĩa là "độ dài chuỗi" + ":" + "nội dung"
    return std::to_string(value.size()) + ':' + value;
}

BencodeValue parseValue(const std::string& input, std::size_t& pos) {
    if (pos >= input.size()) {
        throw std::invalid_argument("Bencode: empty input");
    }

    const char ch = input[pos];
    if (ch == 'i') {
        // Số nguyên theo dạng: i<number>e
        // Ví dụ: i42e = 42, i-10e = -10
        ++pos;
        const std::size_t start = pos;
        while (pos < input.size() && input[pos] != 'e') {
            ++pos;
        }
        if (pos >= input.size()) {
            throw std::invalid_argument("Bencode: unterminated integer");
        }

        const std::string digits = input.substr(start, pos - start);
        if (digits.empty()) {
            throw std::invalid_argument("Bencode: invalid integer format");
        }

        // Cho phép 0 hoặc -0 nhưng không cho số có số 0 ở đầu như 012
        if (digits == "0" || (digits.size() > 1 && digits[0] == '0')) {
            if (digits != "0" && digits != "-0") {
                throw std::invalid_argument("Bencode: invalid integer format");
            }
        }

        try {
            const auto value = std::stoll(digits);
            ++pos;  // bỏ ký tự 'e'
            return BencodeValue(value);
        } catch (const std::exception&) {
            throw std::invalid_argument("Bencode: integer out of range");
        }
    }

    if (ch >= '0' && ch <= '9') {
        // Chuỗi theo dạng: <length>:<content>
        // Ví dụ: "5:hello" = "hello"
        std::size_t length_end = pos;
        while (length_end < input.size() && input[length_end] != ':') {
            ++length_end;
        }
        if (length_end >= input.size()) {
            throw std::invalid_argument("Bencode: invalid string length");
        }

        const std::string length_text = input.substr(pos, length_end - pos);
        if (length_text.empty()) {
            throw std::invalid_argument("Bencode: invalid string length");
        }

        std::size_t length = 0;
        try {
            length = static_cast<std::size_t>(std::stoull(length_text));
        } catch (const std::exception&) {
            throw std::invalid_argument("Bencode: invalid string length");
        }

        const std::size_t data_start = length_end + 1;
        const std::size_t data_end = data_start + length;
        if (data_end > input.size()) {
            throw std::invalid_argument("Bencode: truncated string value");
        }

        const std::string value = input.substr(data_start, length);
        pos = data_end;
        return BencodeValue(value);
    }

    if (ch == 'l') {
        // Danh sách theo dạng: l<item1><item2>...e
        // Ví dụ: l4:spam4:eggse = ["spam", "eggs"]
        ++pos;
        BencodeList list;
        while (pos < input.size() && input[pos] != 'e') {
            list.push_back(parseValue(input, pos));
        }
        if (pos >= input.size()) {
            throw std::invalid_argument("Bencode: unterminated list");
        }
        ++pos;  // bỏ ký tự 'e'
        return BencodeValue(std::move(list));
    }

    if (ch == 'd') {
        // Từ điển theo dạng: d<key><value><key><value>...e
        // Ví dụ: d3:fooi10e3:bar5:helloe = { "foo": 10, "bar": "hello" }
        ++pos;
        BencodeDict dict;
        while (pos < input.size() && input[pos] != 'e') {
            const auto keyValue = parseValue(input, pos);
            const auto* key = keyValue.get_if<std::string>();
            if (key == nullptr) {
                throw std::invalid_argument("Bencode: dictionary keys must be strings");
            }
            const auto value = parseValue(input, pos);
            dict[*key] = value;
        }
        if (pos >= input.size()) {
            throw std::invalid_argument("Bencode: unterminated dictionary");
        }
        ++pos;  // bỏ ký tự 'e'
        return BencodeValue(std::move(dict));
    }

    throw std::invalid_argument("Bencode: unsupported token");
}

std::string encodeBencodeValue(const BencodeValue& value) {
    if (const auto* asInt = value.get_if<int64_t>()) {
        return "i" + std::to_string(*asInt) + "e";
    }
    if (const auto* asString = value.get_if<std::string>()) {
        return encodeStringValue(*asString);
    }
    if (const auto* asList = value.get_if<BencodeList>()) {
        // List: l<encoded_item_1><encoded_item_2>...e
        std::string encoded = "l";
        for (const auto& item : *asList) {
            encoded += encodeBencodeValue(item);
        }
        encoded += "e";
        return encoded;
    }
    if (const auto* asDict = value.get_if<BencodeDict>()) {
        // Dict: d<key1><value1><key2><value2>...e
        // key phải là chuỗi nên ta encode key bằng encodeStringValue(key)
        std::string encoded = "d";
        for (const auto& [key, item] : *asDict) {
            encoded += encodeStringValue(key);
            encoded += encodeBencodeValue(item);
        }
        encoded += "e";
        return encoded;
    }

    throw std::invalid_argument("Bencode: unsupported value type");
}

}  // namespace

BencodeValue Bencode::parse(const std::string& input, std::size_t* next) {
    // Phân tích chuỗi bencode và trả về object C++ tương ứng.
    // Ví dụ: "i42e" -> BencodeValue(42)
    //        "5:hello" -> BencodeValue("hello")
    std::size_t pos = 0;
    const auto value = parseValue(input, pos);
    if (next != nullptr) {
        *next = pos;
    }
    if (pos != input.size()) {
        // Nếu sau khi đọc 1 giá trị mà còn dư ký tự, đó là dữ liệu thừa.
        throw std::invalid_argument("Bencode: trailing bytes after value");
    }
    return value;
}

std::string Bencode::encode(const BencodeValue& value) {
    // Chuyển object C++ về chuỗi Bencode theo chuẩn BitTorrent.
    return encodeBencodeValue(value);
}

BencodeValue decodeBencode(const std::string& input, std::size_t* next) {
    return Bencode::parse(input, next);
}

std::string encodeBencode(const BencodeValue& value) {
    return Bencode::encode(value);
}

std::string encodeBencode(const std::string& value) {
    return encodeStringValue(value);
}

std::string encodeBencode(int64_t value) {
    return Bencode::encode(BencodeValue(value));
}

std::string encodeBencode(const BencodeList& value) {
    return Bencode::encode(BencodeValue(value));
}

std::string encodeBencode(const BencodeDict& value) {
    return Bencode::encode(BencodeValue(value));
}

}  // namespace p2p
