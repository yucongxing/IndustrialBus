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
set_target_properties(industrial_bus_protocol PROPERTIES EXPORT_NAME Protocol)

target_link_libraries(industrial_bus_protocol PUBLIC protobuf::libprotobuf)
target_include_directories(industrial_bus_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

protobuf_generate(
    TARGET industrial_bus_protocol
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

set(INDUSTRIAL_BUS_PROTO_HEADERS)
foreach(proto_file IN LISTS INDUSTRIAL_BUS_PROTO_FILES)
    get_filename_component(proto_name "${proto_file}" NAME_WE)
    list(APPEND INDUSTRIAL_BUS_PROTO_HEADERS
        "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.h")
endforeach()

install(FILES ${INDUSTRIAL_BUS_PROTO_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${INDUSTRIAL_BUS_PROTO_FILES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/industrial-bus/proto)
