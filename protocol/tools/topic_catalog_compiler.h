#pragma once

#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.h>

#include <string>

namespace industrial_bus::catalog_tool {

struct CatalogCompileResult {
    bool ok{false};
    std::string diagnostic;
    std::string public_header;
    std::string runtime_table;
    std::string fingerprint;
};

CatalogCompileResult compileCatalog(
    const catalog::v1::TopicCatalog& catalog,
    const google::protobuf::DescriptorPool& descriptors);

}  // namespace industrial_bus::catalog_tool
