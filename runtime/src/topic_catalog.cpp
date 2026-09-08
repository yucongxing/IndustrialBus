#include "industrial_bus/topic_catalog.h"

#include "industrial_bus/ids.h"
#include "industrial_bus/topics.generated.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace industrial_bus {
namespace {

constexpr TopicBinding kProductionBindings[] = {
#include "topic_catalog.generated.inc"
};

std::vector<std::string_view> segments(std::string_view value) {
    std::vector<std::string_view> result;
    std::size_t offset = 0;
    while (offset <= value.size()) {
        const auto separator = value.find('.', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        result.push_back(value.substr(offset, end - offset));
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1;
    }
    return result;
}

bool isPlaceholder(std::string_view segment) noexcept {
    return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

bool matches(std::string_view pattern, std::string_view topic) {
    if (pattern.find('{') == std::string_view::npos) {
        return pattern == topic;
    }
    const auto pattern_segments = segments(pattern);
    const auto topic_segments = segments(topic);
    if (pattern_segments.size() != topic_segments.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pattern_segments.size(); ++i) {
        if (isPlaceholder(pattern_segments[i])) {
            if (!isValidUuid(topic_segments[i])) {
                return false;
            }
        } else if (pattern_segments[i] != topic_segments[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

const TopicBinding* TopicCatalogView::find(
    std::string_view topic, MessageType message_type,
    CatalogPayloadRole payload_role) const noexcept {
    for (const auto& binding : *this) {
        if (binding.topic_pattern.find('{') == std::string_view::npos &&
            binding.topic_pattern == topic && binding.message_type == message_type &&
            binding.payload_role == payload_role) {
            return &binding;
        }
    }
    for (const auto& binding : *this) {
        if (binding.topic_pattern.find('{') != std::string_view::npos &&
            binding.message_type == message_type &&
            binding.payload_role == payload_role &&
            matches(binding.topic_pattern, topic)) {
            return &binding;
        }
    }
    return nullptr;
}

bool TopicCatalogView::containsTopic(std::string_view topic) const noexcept {
    return std::any_of(begin(), end(), [topic](const TopicBinding& binding) {
        return matches(binding.topic_pattern, topic);
    });
}

bool isValidTopicParameter(std::string_view value,
                           TopicParameterConstraint constraint) noexcept {
    if (value.empty() || value.find('.') != std::string_view::npos ||
        value.find('*') != std::string_view::npos ||
        value.find('{') != std::string_view::npos ||
        value.find('}') != std::string_view::npos) {
        return false;
    }
    return constraint == TopicParameterConstraint::Uuid ? isValidUuid(value)
                                                        : true;
}

TopicCatalogView productionTopicCatalog() noexcept {
    return {kProductionBindings,
            sizeof(kProductionBindings) / sizeof(kProductionBindings[0])};
}

std::string_view productionTopicCatalogFingerprint() noexcept {
    return topics::kCatalogFingerprint;
}

}  // namespace industrial_bus
