#include "compatibility_checker.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

namespace industrial_bus::compat {
namespace {

using google::protobuf::DescriptorProto;
using google::protobuf::EnumDescriptorProto;
using google::protobuf::FileDescriptorProto;

CompatibilityResult incompatible(std::string diagnostic) {
    return {false, std::move(diagnostic)};
}

std::string qualify(const std::string& prefix, const std::string& name) {
    return prefix.empty() ? name : prefix + "." + name;
}

void collectMessages(const DescriptorProto& message, const std::string& prefix,
                     std::map<std::string, const DescriptorProto*>& messages,
                     std::map<std::string, const EnumDescriptorProto*>& enums) {
    const auto full_name = qualify(prefix, message.name());
    messages.emplace(full_name, &message);
    for (const auto& nested : message.nested_type()) {
        collectMessages(nested, full_name, messages, enums);
    }
    for (const auto& nested_enum : message.enum_type()) {
        enums.emplace(qualify(full_name, nested_enum.name()), &nested_enum);
    }
}

void collect(const google::protobuf::FileDescriptorSet& set,
             std::map<std::string, const DescriptorProto*>& messages,
             std::map<std::string, const EnumDescriptorProto*>& enums) {
    for (const auto& file : set.file()) {
        for (const auto& message : file.message_type()) {
            collectMessages(message, file.package(), messages, enums);
        }
        for (const auto& enum_type : file.enum_type()) {
            enums.emplace(qualify(file.package(), enum_type.name()), &enum_type);
        }
    }
}

bool numberReserved(const DescriptorProto& message, int number) {
    return std::any_of(message.reserved_range().begin(),
                       message.reserved_range().end(),
                       [number](const DescriptorProto::ReservedRange& range) {
                           return number >= range.start() && number < range.end();
                       });
}

bool nameReserved(const DescriptorProto& message, const std::string& name) {
    return std::find(message.reserved_name().begin(), message.reserved_name().end(),
                     name) != message.reserved_name().end();
}

using CatalogKey = std::tuple<std::string, int, int, std::uint32_t>;

CatalogKey key(const catalog::v1::TopicBinding& binding) {
    return {binding.topic_pattern(), static_cast<int>(binding.message_type()),
            static_cast<int>(binding.payload_role()), binding.schema_major()};
}

std::string keyName(const CatalogKey& value) {
    std::ostringstream output;
    output << std::get<0>(value) << " [type=" << std::get<1>(value)
           << ", role=" << std::get<2>(value)
           << ", schema=" << std::get<3>(value) << ']';
    return output.str();
}

std::string parameters(const catalog::v1::TopicBinding& binding) {
    std::ostringstream output;
    for (const auto& parameter : binding.parameters()) {
        output << parameter.name() << ':'
               << static_cast<int>(parameter.constraint()) << ';';
    }
    return output.str();
}

}  // namespace

CompatibilityResult checkDescriptors(
    const google::protobuf::FileDescriptorSet& baseline,
    const google::protobuf::FileDescriptorSet& current) {
    std::map<std::string, const DescriptorProto*> old_messages;
    std::map<std::string, const DescriptorProto*> new_messages;
    std::map<std::string, const EnumDescriptorProto*> old_enums;
    std::map<std::string, const EnumDescriptorProto*> new_enums;
    collect(baseline, old_messages, old_enums);
    collect(current, new_messages, new_enums);

    for (const auto& item : old_messages) {
        const auto found_message = new_messages.find(item.first);
        if (found_message == new_messages.end()) {
            return incompatible("published message removed: " + item.first);
        }
        const auto& old_message = *item.second;
        const auto& new_message = *found_message->second;
        std::map<int, const google::protobuf::FieldDescriptorProto*> new_fields;
        for (const auto& field : new_message.field()) {
            new_fields.emplace(field.number(), &field);
        }
        for (const auto& old_field : old_message.field()) {
            const auto found_field = new_fields.find(old_field.number());
            if (found_field == new_fields.end()) {
                if (!numberReserved(new_message, old_field.number()) ||
                    !nameReserved(new_message, old_field.name())) {
                    return incompatible(
                        item.first + "." + old_field.name() + " field " +
                        std::to_string(old_field.number()) +
                        " was removed without reserving both name and number");
                }
                continue;
            }
            const auto& new_field = *found_field->second;
            if (old_field.name() != new_field.name()) {
                return incompatible(item.first + " field " +
                                    std::to_string(old_field.number()) +
                                    " was reused from '" + old_field.name() +
                                    "' as '" + new_field.name() + "'");
            }
            if (old_field.type() != new_field.type() ||
                old_field.type_name() != new_field.type_name() ||
                old_field.label() != new_field.label() ||
                old_field.has_oneof_index() != new_field.has_oneof_index() ||
                old_field.oneof_index() != new_field.oneof_index() ||
                old_field.proto3_optional() != new_field.proto3_optional()) {
                return incompatible(item.first + "." + old_field.name() +
                                    " field " +
                                    std::to_string(old_field.number()) +
                                    " changed type, cardinality, or oneof presence");
            }
        }
    }

    for (const auto& item : old_enums) {
        const auto found_enum = new_enums.find(item.first);
        if (found_enum == new_enums.end()) {
            return incompatible("published enum removed: " + item.first);
        }
        std::map<int, std::string> current_values;
        for (const auto& value : found_enum->second->value()) {
            current_values.emplace(value.number(), value.name());
        }
        for (const auto& old_value : item.second->value()) {
            const auto found_value = current_values.find(old_value.number());
            if (found_value == current_values.end()) {
                return incompatible(item.first + "." + old_value.name() +
                                    " enum value " +
                                    std::to_string(old_value.number()) +
                                    " was removed");
            }
            if (found_value->second != old_value.name()) {
                return incompatible(item.first + " enum value " +
                                    std::to_string(old_value.number()) +
                                    " was renamed or renumbered from '" +
                                    old_value.name() + "' to '" +
                                    found_value->second + "'");
            }
        }
    }
    return {};
}

CompatibilityResult checkCatalog(
    const catalog::v1::TopicCatalog& baseline,
    const catalog::v1::TopicCatalog& current) {
    std::map<CatalogKey, const catalog::v1::TopicBinding*> current_bindings;
    for (const auto& binding : current.bindings()) {
        current_bindings.emplace(key(binding), &binding);
    }
    for (const auto& old_binding : baseline.bindings()) {
        const auto old_key = key(old_binding);
        const auto found = current_bindings.find(old_key);
        if (found == current_bindings.end()) {
            return incompatible("published catalog binding removed: " +
                                keyName(old_key));
        }
        const auto& new_binding = *found->second;
        if (old_binding.payload_type() != new_binding.payload_type() ||
            old_binding.validator() != new_binding.validator() ||
            parameters(old_binding) != parameters(new_binding)) {
            return incompatible("catalog binding changed in place: " +
                                keyName(old_key) + " old payload='" +
                                old_binding.payload_type() + "' new payload='" +
                                new_binding.payload_type() + "'");
        }
        if (new_binding.max_payload_bytes() < old_binding.max_payload_bytes()) {
            return incompatible("catalog payload limit narrowed in place: " +
                                keyName(old_key));
        }
    }
    return {};
}

}  // namespace industrial_bus::compat
