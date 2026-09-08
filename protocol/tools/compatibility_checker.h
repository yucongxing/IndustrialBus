#pragma once

#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.pb.h>

#include <string>

namespace industrial_bus::compat {

struct CompatibilityResult {
    bool compatible{true};
    std::string diagnostic;
};

CompatibilityResult checkDescriptors(
    const google::protobuf::FileDescriptorSet& baseline,
    const google::protobuf::FileDescriptorSet& current);

CompatibilityResult checkCatalog(
    const catalog::v1::TopicCatalog& baseline,
    const catalog::v1::TopicCatalog& current);

}  // namespace industrial_bus::compat
