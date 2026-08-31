if(DEFINED INDUSTRIAL_BUS_ENABLE_PROTOBUF AND NOT INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    return()
endif()

if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf CONFIG QUIET)
    if(NOT TARGET protobuf::libprotobuf)
        find_package(Protobuf REQUIRED)
    endif()
endif()

set(INDUSTRIAL_BUS_PROTO_FILES
    proto/envelope.proto
    proto/system.proto
    proto/operation_state.proto
    proto/operation.proto
    proto/logging.proto
    proto/data_ref.proto
    proto/layout.proto
    proto/cmp.proto
)

add_library(industrial_bus_protocol STATIC ${INDUSTRIAL_BUS_PROTO_FILES})
add_library(IndustrialBus::Protocol ALIAS industrial_bus_protocol)

target_link_libraries(industrial_bus_protocol PUBLIC protobuf::libprotobuf)
target_include_directories(industrial_bus_protocol
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

protobuf_generate(
    TARGET industrial_bus_protocol
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
