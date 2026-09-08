#include "topic_catalog_compiler.h"

#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string catalog;
    std::string descriptor_set;
    std::string public_output;
    std::string runtime_output;
    bool check{false};
};

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--check") {
            options.check = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const std::string value = argv[++i];
        if (argument == "--catalog") options.catalog = value;
        else if (argument == "--descriptor-set") options.descriptor_set = value;
        else if (argument == "--public-out") options.public_output = value;
        else if (argument == "--runtime-out") options.runtime_output = value;
        else return false;
    }
    return !options.catalog.empty() && !options.descriptor_set.empty() &&
           !options.public_output.empty() && !options.runtime_output.empty();
}

bool readBinary(const std::string& path, std::string& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output && static_cast<bool>(output.write(content.data(),
                                                     static_cast<std::streamsize>(content.size())));
}

bool loadDescriptors(const std::string& path,
                     google::protobuf::DescriptorPool& pool) {
    std::string bytes;
    google::protobuf::FileDescriptorSet set;
    if (!readBinary(path, bytes) || !set.ParseFromString(bytes)) return false;
    std::vector<bool> loaded(static_cast<std::size_t>(set.file_size()), false);
    int remaining = set.file_size();
    while (remaining > 0) {
        bool progress = false;
        for (int i = 0; i < set.file_size(); ++i) {
            if (loaded[static_cast<std::size_t>(i)]) continue;
            if (pool.BuildFile(set.file(i)) != nullptr) {
                loaded[static_cast<std::size_t>(i)] = true;
                --remaining;
                progress = true;
            }
        }
        if (!progress) return false;
    }
    return true;
}

bool compare(const std::string& path, const std::string& expected) {
    std::string current;
    return readBinary(path, current) && current == expected;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::cerr << "usage: topic_catalog_generator --catalog FILE --descriptor-set FILE "
                     "--public-out FILE --runtime-out FILE [--check]\n";
        return 2;
    }
    industrial_bus::catalog::v1::TopicCatalog catalog;
    std::ifstream catalog_input(options.catalog);
    google::protobuf::io::IstreamInputStream catalog_stream(&catalog_input);
    if (!catalog_input ||
        !google::protobuf::TextFormat::Parse(&catalog_stream, &catalog)) {
        std::cerr << "failed to parse Topic Catalog: " << options.catalog << '\n';
        return 1;
    }
    google::protobuf::DescriptorPool descriptors;
    if (!loadDescriptors(options.descriptor_set, descriptors)) {
        std::cerr << "failed to load descriptor set: " << options.descriptor_set << '\n';
        return 1;
    }
    const auto compiled = industrial_bus::catalog_tool::compileCatalog(catalog,
                                                                        descriptors);
    if (!compiled.ok) {
        std::cerr << compiled.diagnostic << '\n';
        return 1;
    }
    if (options.check) {
        if (!compare(options.public_output, compiled.public_header) ||
            !compare(options.runtime_output, compiled.runtime_table)) {
            std::cerr << "generated Topic Catalog outputs are stale; run target "
                         "industrial_bus_generate_topic_catalog\n";
            return 1;
        }
        return 0;
    }
    if (!writeFile(options.public_output, compiled.public_header) ||
        !writeFile(options.runtime_output, compiled.runtime_table)) {
        std::cerr << "failed to write generated Topic Catalog outputs\n";
        return 1;
    }
    std::cout << compiled.fingerprint << '\n';
    return 0;
}
