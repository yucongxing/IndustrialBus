#pragma once

#include "industrial_bus/bootstrap/runtime_paths.h"

#include <cstdint>
#include <string>

namespace industrial_bus::bootstrap {

struct EndpointRecord {
    std::string endpoint;
    std::int64_t pid{0};
    std::string hub_instance_id;
    std::uint32_t protocol_version{0};
    std::int64_t start_time{0};
};

void writeEndpointRecord(const RuntimePaths& paths, const EndpointRecord& record);
EndpointRecord readEndpointRecord(const RuntimePaths& paths);

// Generates a 256-bit token encoded as 64 lowercase hexadecimal characters.
std::string generateAuthToken();
void writeAuthToken(const RuntimePaths& paths, const std::string& token);
std::string readAuthToken(const RuntimePaths& paths);

}  // namespace industrial_bus::bootstrap
