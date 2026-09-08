#pragma once

#include "industrial_bus/qos.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace industrial_bus {

enum class CatalogPayloadRole : std::uint8_t {
    Unspecified = 0,
    CommandInput = 1,
    CommandAck = 2,
    RequestInput = 3,
    ResponseOutput = 4,
    EventFact = 5,
    ErrorDetail = 6,
    SystemPayload = 7,
};

enum class TopicParameterConstraint : std::uint8_t {
    None,
    Uuid,
};

enum class ProtocolPayloadValidatorKind : std::uint8_t {
    None,
    OperationProgressId,
};

struct TopicBinding {
    std::string_view symbol;
    std::string_view topic_pattern;
    MessageType message_type{MessageType::Event};
    CatalogPayloadRole payload_role{CatalogPayloadRole::Unspecified};
    std::string_view payload_type;
    std::uint32_t schema_major{1};
    std::size_t max_payload_bytes{0};
    ProtocolPayloadValidatorKind validator{ProtocolPayloadValidatorKind::None};
};

class TopicCatalogView {
public:
    constexpr TopicCatalogView() noexcept = default;
    constexpr TopicCatalogView(const TopicBinding* bindings,
                               std::size_t size) noexcept
        : bindings_(bindings), size_(size) {}

    const TopicBinding* find(std::string_view topic,
                             MessageType message_type,
                             CatalogPayloadRole payload_role) const noexcept;
    bool containsTopic(std::string_view topic) const noexcept;
    const TopicBinding* begin() const noexcept { return bindings_; }
    const TopicBinding* end() const noexcept { return bindings_ + size_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    const TopicBinding* bindings_{nullptr};
    std::size_t size_{0};
};

bool isValidTopicParameter(std::string_view value,
                           TopicParameterConstraint constraint) noexcept;
TopicCatalogView productionTopicCatalog() noexcept;
std::string_view productionTopicCatalogFingerprint() noexcept;

}  // namespace industrial_bus
