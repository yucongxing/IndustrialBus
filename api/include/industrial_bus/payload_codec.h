#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace industrial_bus {

using ByteBuffer = std::vector<std::uint8_t>;

namespace detail {
template <typename T, typename = void>
struct HasProtobufSerialize : std::false_type {};

template <typename T>
struct HasProtobufSerialize<T, std::void_t<decltype(
    std::declval<const T&>().SerializeToString(std::declval<std::string*>()))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasProtobufParse : std::false_type {};

template <typename T>
struct HasProtobufParse<T, std::void_t<decltype(
    std::declval<T&>().ParseFromArray(nullptr, int{}))>> : std::true_type {};
}  // namespace detail

// Applications may specialize PayloadCodec for non-Protobuf payloads.
template <typename T, typename Enable = void>
struct PayloadCodec {
    static bool encode(const T& value, ByteBuffer& output) {
        static_assert(detail::HasProtobufSerialize<T>::value,
                      "Payload must be a protobuf message or provide PayloadCodec<T>");
        std::string bytes;
        if (!value.SerializeToString(&bytes)) {
            return false;
        }
        output.assign(bytes.begin(), bytes.end());
        return true;
    }

    static bool decode(const ByteBuffer& input, T& value) {
        static_assert(detail::HasProtobufParse<T>::value,
                      "Payload must be a protobuf message or provide PayloadCodec<T>");
        return value.ParseFromArray(input.data(), static_cast<int>(input.size()));
    }
};

template <>
struct PayloadCodec<std::string> {
    static bool encode(const std::string& value, ByteBuffer& output) {
        output.assign(value.begin(), value.end());
        return true;
    }
    static bool decode(const ByteBuffer& input, std::string& value) {
        value.assign(input.begin(), input.end());
        return true;
    }
};

template <>
struct PayloadCodec<ByteBuffer> {
    static bool encode(const ByteBuffer& value, ByteBuffer& output) {
        output = value;
        return true;
    }
    static bool decode(const ByteBuffer& input, ByteBuffer& value) {
        value = input;
        return true;
    }
};

}  // namespace industrial_bus
