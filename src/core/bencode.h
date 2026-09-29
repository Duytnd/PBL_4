#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace p2p {

struct BencodeValue {
    using Storage = std::variant<
        int64_t,
        std::string,
        std::vector<BencodeValue>,
        std::map<std::string, BencodeValue>>;

    Storage data;

    BencodeValue() = default;
    BencodeValue(int64_t value) : data(value) {}
    BencodeValue(const std::string& value) : data(value) {}
    BencodeValue(std::string&& value) : data(std::move(value)) {}
    BencodeValue(std::vector<BencodeValue> value) : data(std::move(value)) {}
    BencodeValue(std::map<std::string, BencodeValue> value) : data(std::move(value)) {}

    bool operator==(const BencodeValue& other) const {
        return data == other.data;
    }

    bool operator!=(const BencodeValue& other) const {
        return !(*this == other);
    }

    template <typename T>
    const T* get_if() const {
        return std::get_if<T>(&data);
    }
};

using BencodeList = std::vector<BencodeValue>;
using BencodeDict = std::map<std::string, BencodeValue>;

class Bencode {
public:
    static BencodeValue parse(const std::string& input, std::size_t* next = nullptr);
    static std::string encode(const BencodeValue& value);
};

BencodeValue decodeBencode(const std::string& input, std::size_t* next = nullptr);
std::string encodeBencode(const BencodeValue& value);
std::string encodeBencode(const std::string& value);
std::string encodeBencode(int64_t value);
std::string encodeBencode(const BencodeList& value);
std::string encodeBencode(const BencodeDict& value);

}  // namespace p2p
