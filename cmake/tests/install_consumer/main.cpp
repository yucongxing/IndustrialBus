#include "industrial_bus/bus_client.h"

#include "operation.pb.h"

#include <string>

int main() {
    industrial_bus::operation::v1::OperationEvent event;
    event.set_operation_id("install-consumer-operation");
    std::string encoded;
    if (!event.SerializeToString(&encoded) || encoded.empty()) {
        return 1;
    }

    industrial_bus::ClientOptions invalid_options;
    const auto connected = industrial_bus::BusClient::connect(invalid_options);
    if (connected ||
        connected.error().category !=
            industrial_bus::ErrorCategory::InvalidArgument) {
        return 2;
    }
    return 0;
}
