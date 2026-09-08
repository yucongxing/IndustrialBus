#include "compatibility_checker.h"

#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool readDescriptor(const std::string& path,
                    google::protobuf::FileDescriptorSet& descriptor) {
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    return input && descriptor.ParseFromString(bytes);
}

bool readCatalog(const std::string& path,
                 industrial_bus::catalog::v1::TopicCatalog& catalog) {
    std::ifstream input(path);
    google::protobuf::io::IstreamInputStream stream(&input);
    return input && google::protobuf::TextFormat::Parse(&stream, &catalog);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: protocol_compatibility_check BASELINE_DESCRIPTOR "
                     "CURRENT_DESCRIPTOR BASELINE_CATALOG CURRENT_CATALOG\n";
        return 2;
    }
    google::protobuf::FileDescriptorSet baseline_descriptor;
    google::protobuf::FileDescriptorSet current_descriptor;
    industrial_bus::catalog::v1::TopicCatalog baseline_catalog;
    industrial_bus::catalog::v1::TopicCatalog current_catalog;
    if (!readDescriptor(argv[1], baseline_descriptor) ||
        !readDescriptor(argv[2], current_descriptor) ||
        !readCatalog(argv[3], baseline_catalog) ||
        !readCatalog(argv[4], current_catalog)) {
        std::cerr << "failed to read compatibility input\n";
        return 1;
    }
    const auto descriptor_result = industrial_bus::compat::checkDescriptors(
        baseline_descriptor, current_descriptor);
    if (!descriptor_result.compatible) {
        std::cerr << descriptor_result.diagnostic << '\n';
        return 1;
    }
    const auto catalog_result = industrial_bus::compat::checkCatalog(
        baseline_catalog, current_catalog);
    if (!catalog_result.compatible) {
        std::cerr << catalog_result.diagnostic << '\n';
        return 1;
    }
    return 0;
}
