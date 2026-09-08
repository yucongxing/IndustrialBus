#include "topic_catalog_compiler.h"

#include "envelope.pb.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace industrial_bus::catalog_tool {
namespace {

namespace catalog_proto = industrial_bus::catalog::v1;
namespace envelope = industrial_bus::protocol::v1;

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset <= value.size()) {
        const auto separator = value.find('.', offset);
        const auto end = separator == std::string::npos ? value.size() : separator;
        result.push_back(value.substr(offset, end - offset));
        if (separator == std::string::npos) break;
        offset = separator + 1;
    }
    return result;
}

bool placeholder(const std::string& segment) {
    return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

bool validSymbol(const std::string& value) {
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) ||
          value.front() == '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char value_char) {
        return std::isalnum(static_cast<unsigned char>(value_char)) ||
               value_char == '_';
    });
}

bool patternsOverlap(const catalog_proto::TopicBinding& lhs,
                     const catalog_proto::TopicBinding& rhs) {
    const auto left = split(lhs.topic_pattern());
    const auto right = split(rhs.topic_pattern());
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!placeholder(left[i]) && !placeholder(right[i]) && left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

std::string messageTypeName(envelope::MessageType type) {
    switch (type) {
        case envelope::COMMAND: return "MessageType::Command";
        case envelope::EVENT: return "MessageType::Event";
        case envelope::REQUEST: return "MessageType::Request";
        case envelope::REPLY: return "MessageType::Reply";
        case envelope::PROGRESS: return "MessageType::Progress";
        case envelope::ERROR: return "MessageType::Error";
        case envelope::LOG: return "MessageType::Log";
        case envelope::HEARTBEAT: return "MessageType::Heartbeat";
        case envelope::SYSTEM: return "MessageType::System";
        case envelope::MESSAGE_TYPE_UNSPECIFIED: break;
        default: break;
    }
    return "MessageType::Event";
}

std::string payloadRoleName(envelope::PayloadRole role) {
    switch (role) {
        case envelope::COMMAND_INPUT: return "CatalogPayloadRole::CommandInput";
        case envelope::COMMAND_ACK: return "CatalogPayloadRole::CommandAck";
        case envelope::REQUEST_INPUT: return "CatalogPayloadRole::RequestInput";
        case envelope::RESPONSE_OUTPUT: return "CatalogPayloadRole::ResponseOutput";
        case envelope::EVENT_FACT: return "CatalogPayloadRole::EventFact";
        case envelope::ERROR_DETAIL: return "CatalogPayloadRole::ErrorDetail";
        case envelope::SYSTEM_PAYLOAD: return "CatalogPayloadRole::SystemPayload";
        case envelope::PAYLOAD_ROLE_UNSPECIFIED: break;
        default: break;
    }
    return "CatalogPayloadRole::Unspecified";
}

std::string validatorName(catalog_proto::ProtocolValidatorKind validator) {
    return validator == catalog_proto::OPERATION_PROGRESS_ID
               ? "ProtocolPayloadValidatorKind::OperationProgressId"
               : "ProtocolPayloadValidatorKind::None";
}

std::string cppType(std::string full_name) {
    constexpr std::string_view prefix = "industrial_bus.";
    if (full_name.compare(0, prefix.size(), prefix) == 0) {
        full_name.erase(0, prefix.size());
    }
    std::size_t offset = 0;
    while ((offset = full_name.find('.', offset)) != std::string::npos) {
        full_name.replace(offset, 1, "::");
        offset += 2;
    }
    return full_name;
}

std::string messageClassName(envelope::MessageType type) {
    switch (type) {
        case envelope::PROGRESS: return "MessageClass::Progress";
        case envelope::LOG: return "MessageClass::InfoLog";
        case envelope::EVENT: return "MessageClass::Event";
        default: return "MessageClass::Event";
    }
}

std::string canonical(const catalog_proto::TopicCatalog& input) {
    std::vector<const catalog_proto::TopicBinding*> bindings;
    bindings.reserve(static_cast<std::size_t>(input.bindings_size()));
    for (const auto& binding : input.bindings()) bindings.push_back(&binding);
    std::sort(bindings.begin(), bindings.end(), [](const auto* lhs, const auto* rhs) {
        return std::make_tuple(lhs->topic_pattern(), lhs->message_type(),
                               lhs->payload_role(), lhs->payload_type(),
                               lhs->symbol()) <
               std::make_tuple(rhs->topic_pattern(), rhs->message_type(),
                               rhs->payload_role(), rhs->payload_type(),
                               rhs->symbol());
    });
    std::ostringstream output;
    output << input.catalog_version() << '\n';
    for (const auto* binding : bindings) {
        output << binding->symbol() << '|' << binding->topic_pattern() << '|'
               << static_cast<int>(binding->message_type()) << '|'
               << static_cast<int>(binding->payload_role()) << '|'
               << binding->payload_type() << '|' << binding->schema_major() << '|'
               << binding->max_payload_bytes() << '|'
               << static_cast<int>(binding->validator()) << '\n';
    }
    return output.str();
}

std::string fingerprint(const std::string& input) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : input) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string renderRuntime(const catalog_proto::TopicCatalog& input,
                          const std::string& hash) {
    std::ostringstream output;
    output << "// Generated from protocol/catalog/topics.textproto. Do not edit by hand.\n"
           << "// Catalog fingerprint: " << hash << "\n";
    for (const auto& binding : input.bindings()) {
        output << "{\"" << binding.symbol() << "\", \""
               << binding.topic_pattern() << "\", "
               << messageTypeName(binding.message_type()) << ", "
               << payloadRoleName(binding.payload_role()) << ", \""
               << binding.payload_type() << "\", " << binding.schema_major()
               << ", " << binding.max_payload_bytes() << ", "
               << validatorName(binding.validator()) << "},\n";
    }
    return output.str();
}

std::string renderPublic(const catalog_proto::TopicCatalog& input,
                         const std::string& hash) {
    std::ostringstream output;
    output << R"cpp(// Generated from protocol/catalog/topics.textproto. Do not edit by hand.
#pragma once

#include "industrial_bus/ids.h"
#include "industrial_bus/topic.h"

#include "cmp.pb.h"
#include "logging.pb.h"
#include "operation.pb.h"
#include "system.pb.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace industrial_bus::topics {

)cpp";
    output << "inline constexpr std::string_view kCatalogFingerprint = \"" << hash
           << "\";\n\n";
    for (const auto& binding : input.bindings()) {
        if (!binding.parameters().empty()) {
            output << "inline constexpr std::string_view " << binding.symbol()
                   << "Pattern = \"" << binding.topic_pattern() << "\";\n";
            continue;
        }
        if (binding.message_type() == envelope::COMMAND ||
            binding.message_type() == envelope::REQUEST) {
            const auto success_role = binding.message_type() == envelope::COMMAND
                                          ? envelope::COMMAND_ACK
                                          : envelope::RESPONSE_OUTPUT;
            const catalog_proto::TopicBinding* success = nullptr;
            for (const auto& candidate : input.bindings()) {
                if (candidate.topic_pattern() == binding.topic_pattern() &&
                    candidate.message_type() == envelope::REPLY &&
                    candidate.payload_role() == success_role) {
                    success = &candidate;
                    break;
                }
            }
            if (success != nullptr) {
                output << "inline constexpr "
                       << (binding.message_type() == envelope::COMMAND
                               ? "CommandTopic<"
                               : "RequestTopic<")
                       << cppType(binding.payload_type()) << ", "
                       << cppType(success->payload_type()) << "> "
                       << binding.symbol() << "{\n    \""
                       << binding.topic_pattern() << "\", \""
                       << binding.payload_type() << "\", \""
                       << success->payload_type() << "\"};\n";
                continue;
            }
        }
        if (binding.message_type() == envelope::EVENT ||
            binding.message_type() == envelope::PROGRESS ||
            binding.message_type() == envelope::LOG) {
            output << "inline constexpr EventTopic<"
                   << cppType(binding.payload_type()) << "> " << binding.symbol()
                   << "{\n    \"" << binding.topic_pattern() << "\", \""
                   << binding.payload_type() << "\", "
                   << messageClassName(binding.message_type()) << "};\n";
        } else {
            output << "inline constexpr SystemTopic<"
                   << cppType(binding.payload_type()) << "> " << binding.symbol()
                   << "{\n    \"" << binding.topic_pattern() << "\", \""
                   << binding.payload_type() << "\", "
                   << messageTypeName(binding.message_type()) << "};\n";
        }
    }
    output << "\n";
    output << R"cpp(class ChannelTopics {
public:
    explicit ChannelTopics(std::string channel_uuid)
        : channel_uuid_(std::move(channel_uuid)),
          prefix_("cmp.channel." + channel_uuid_),
          run_(prefix_ + ".simulation.run"),
          progress_(prefix_ + ".simulation.progress"),
          completed_(prefix_ + ".simulation.completed"),
          failed_(prefix_ + ".simulation.failed"),
          log_("application.log." + channel_uuid_) {
        if (!isValidUuid(channel_uuid_)) {
            throw std::invalid_argument("channel_uuid must be an RFC 4122 UUID segment");
        }
    }

    ChannelTopics(const ChannelTopics&) = delete;
    ChannelTopics& operator=(const ChannelTopics&) = delete;
    ChannelTopics(ChannelTopics&&) = delete;
    ChannelTopics& operator=(ChannelTopics&&) = delete;

    const std::string& channelId() const noexcept { return channel_uuid_; }

    CommandTopic<cmp::v1::RunSimulation, system::v1::CommandAck>
    runSimulation() const noexcept {
        return {run_, "industrial_bus.cmp.v1.RunSimulation",
                "industrial_bus.system.v1.CommandAck"};
    }

    EventTopic<cmp::v1::SimulationProgress> simulationProgress() const noexcept {
        return {progress_, "industrial_bus.cmp.v1.SimulationProgress",
                MessageClass::Progress};
    }

    EventTopic<cmp::v1::SimulationCompleted> simulationCompleted() const noexcept {
        return {completed_, "industrial_bus.cmp.v1.SimulationCompleted",
                MessageClass::FinalState};
    }

    EventTopic<cmp::v1::SimulationFailed> simulationFailed() const noexcept {
        return {failed_, "industrial_bus.cmp.v1.SimulationFailed",
                MessageClass::FinalState};
    }

    EventTopic<logging::v1::LogEvent> applicationLog() const noexcept {
        return {log_, "industrial_bus.logging.v1.LogEvent",
                MessageClass::InfoLog};
    }

private:
    std::string channel_uuid_;
    std::string prefix_;
    std::string run_;
    std::string progress_;
    std::string completed_;
    std::string failed_;
    std::string log_;
};

}  // namespace industrial_bus::topics
)cpp";
    return output.str();
}

CatalogCompileResult invalid(std::string diagnostic) {
    return {false, std::move(diagnostic), {}, {}, {}};
}

}  // namespace

CatalogCompileResult compileCatalog(
    const catalog::v1::TopicCatalog& input,
    const google::protobuf::DescriptorPool& descriptors) {
    if (input.catalog_version() == 0 || input.bindings().empty()) {
        return invalid("catalog_version and at least one binding are required");
    }
    std::set<std::tuple<std::string, int, int>> keys;
    std::set<std::string> symbols;
    for (const auto& binding : input.bindings()) {
        if (!validSymbol(binding.symbol()) || binding.topic_pattern().empty() ||
            binding.message_type() == envelope::MESSAGE_TYPE_UNSPECIFIED ||
            binding.payload_role() == envelope::PAYLOAD_ROLE_UNSPECIFIED ||
            binding.payload_type().empty() || binding.schema_major() == 0 ||
            binding.max_payload_bytes() == 0) {
            return invalid("binding '" + binding.symbol() +
                           "' is missing required metadata");
        }
        if (descriptors.FindMessageTypeByName(binding.payload_type()) == nullptr) {
            return invalid("binding '" + binding.symbol() + "' for Topic '" +
                           binding.topic_pattern() + "' references missing payload '" +
                           binding.payload_type() + "'");
        }
        if (!symbols.insert(binding.symbol()).second) {
            return invalid("duplicate C++ symbol '" + binding.symbol() + "'");
        }
        const auto key = std::make_tuple(binding.topic_pattern(),
                                         static_cast<int>(binding.message_type()),
                                         static_cast<int>(binding.payload_role()));
        if (!keys.insert(key).second) {
            return invalid("duplicate binding key for Topic '" +
                           binding.topic_pattern() + "'");
        }
        std::map<std::string, catalog_proto::ParameterConstraint> parameters;
        for (const auto& parameter : binding.parameters()) {
            if (parameter.name().empty() ||
                parameter.constraint() ==
                    catalog_proto::PARAMETER_CONSTRAINT_UNSPECIFIED ||
                !parameters.emplace(parameter.name(), parameter.constraint()).second) {
                return invalid("invalid or duplicate parameter in binding '" +
                               binding.symbol() + "'");
            }
        }
        std::set<std::string> used;
        for (const auto& segment : split(binding.topic_pattern())) {
            if (segment.empty()) {
                return invalid("empty Topic segment in binding '" + binding.symbol() + "'");
            }
            if (placeholder(segment)) {
                const auto name = segment.substr(1, segment.size() - 2);
                if (parameters.count(name) == 0 || !used.insert(name).second) {
                    return invalid("undeclared or repeated Topic parameter '" + name +
                                   "' in binding '" + binding.symbol() + "'");
                }
            } else if (segment.find('{') != std::string::npos ||
                       segment.find('}') != std::string::npos ||
                       segment.find('*') != std::string::npos) {
                return invalid("unsafe Topic segment in binding '" +
                               binding.symbol() + "'");
            }
        }
        if (used.size() != parameters.size()) {
            return invalid("unused Topic parameter in binding '" + binding.symbol() + "'");
        }
    }

    for (int i = 0; i < input.bindings_size(); ++i) {
        const auto& lhs = input.bindings(i);
        if (lhs.parameters().empty()) continue;
        for (int j = i + 1; j < input.bindings_size(); ++j) {
            const auto& rhs = input.bindings(j);
            if (rhs.parameters().empty() || lhs.message_type() != rhs.message_type() ||
                lhs.payload_role() != rhs.payload_role()) {
                continue;
            }
            if (patternsOverlap(lhs, rhs)) {
                return invalid("ambiguous Topic patterns '" + lhs.topic_pattern() +
                               "' and '" + rhs.topic_pattern() + "'");
            }
        }
    }

    for (const auto& binding : input.bindings()) {
        if (binding.message_type() != envelope::COMMAND &&
            binding.message_type() != envelope::REQUEST) {
            continue;
        }
        const auto success_role = binding.message_type() == envelope::COMMAND
                                      ? envelope::COMMAND_ACK
                                      : envelope::RESPONSE_OUTPUT;
        bool success = false;
        bool error = false;
        for (const auto& candidate : input.bindings()) {
            if (candidate.topic_pattern() != binding.topic_pattern() ||
                candidate.message_type() != envelope::REPLY) {
                continue;
            }
            success = success || candidate.payload_role() == success_role;
            error = error || candidate.payload_role() == envelope::ERROR_DETAIL;
        }
        if (!success || !error) {
            return invalid("bidirectional Topic '" + binding.topic_pattern() +
                           "' requires success and Error Reply bindings");
        }
    }

    const auto hash = fingerprint(canonical(input));
    return {true, {}, renderPublic(input, hash), renderRuntime(input, hash), hash};
}

}  // namespace industrial_bus::catalog_tool
