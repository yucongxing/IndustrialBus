#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace industrial_bus {

template <typename Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(std::string value) : value_(std::move(value)) {}

    const std::string& value() const noexcept { return value_; }
    bool empty() const noexcept { return value_.empty(); }
    explicit operator bool() const noexcept { return !empty(); }

    friend bool operator==(const StrongId& lhs, const StrongId& rhs) {
        return lhs.value_ == rhs.value_;
    }
    friend bool operator!=(const StrongId& lhs, const StrongId& rhs) {
        return !(lhs == rhs);
    }
    friend bool operator<(const StrongId& lhs, const StrongId& rhs) {
        return lhs.value_ < rhs.value_;
    }

private:
    std::string value_;
};

struct MessageIdTag;
struct CorrelationIdTag;
struct OperationIdTag;
struct InstanceIdTag;

using MessageId = StrongId<MessageIdTag>;
using CorrelationId = StrongId<CorrelationIdTag>;
using OperationId = StrongId<OperationIdTag>;
using InstanceId = StrongId<InstanceIdTag>;

std::string generateUuidV4();
bool isValidUuid(std::string_view value) noexcept;
bool isValidProtocolId(std::string_view value, std::size_t max_length = 128) noexcept;

template <typename Tag>
StrongId<Tag> generateStrongId() {
    return StrongId<Tag>(generateUuidV4());
}

}  // namespace industrial_bus

namespace std {
template <typename Tag>
struct hash<industrial_bus::StrongId<Tag>> {
    std::size_t operator()(const industrial_bus::StrongId<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};
}  // namespace std
