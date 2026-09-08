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
    proto/topic_catalog.proto
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

set(INDUSTRIAL_BUS_DESCRIPTOR_SET
    "${CMAKE_CURRENT_BINARY_DIR}/industrial_bus_current.pb")
set(INDUSTRIAL_BUS_PROTO_ABSOLUTE_FILES)
foreach(proto_file IN LISTS INDUSTRIAL_BUS_PROTO_FILES)
    list(APPEND INDUSTRIAL_BUS_PROTO_ABSOLUTE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}")
endforeach()
add_custom_command(
    OUTPUT "${INDUSTRIAL_BUS_DESCRIPTOR_SET}"
    COMMAND protobuf::protoc
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --include_imports
        --descriptor_set_out=${INDUSTRIAL_BUS_DESCRIPTOR_SET}
        ${INDUSTRIAL_BUS_PROTO_ABSOLUTE_FILES}
    DEPENDS ${INDUSTRIAL_BUS_PROTO_FILES}
    VERBATIM)
add_custom_target(industrial_bus_protocol_descriptor
    DEPENDS "${INDUSTRIAL_BUS_DESCRIPTOR_SET}")

add_library(industrial_bus_topic_catalog_compiler STATIC
    tools/topic_catalog_compiler.cpp)
target_include_directories(industrial_bus_topic_catalog_compiler
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(industrial_bus_topic_catalog_compiler
    PUBLIC IndustrialBus::Protocol protobuf::libprotobuf)
industrial_bus_target_defaults(industrial_bus_topic_catalog_compiler)

add_executable(industrial_bus_topic_catalog_generator
    tools/topic_catalog_generator.cpp)
target_link_libraries(industrial_bus_topic_catalog_generator
    PRIVATE industrial_bus_topic_catalog_compiler)
industrial_bus_target_defaults(industrial_bus_topic_catalog_generator)
add_dependencies(industrial_bus_topic_catalog_generator
    industrial_bus_protocol_descriptor)

set(INDUSTRIAL_BUS_CATALOG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog/topics.textproto")

add_library(industrial_bus_protocol_compatibility STATIC
    tools/compatibility_checker.cpp)
target_include_directories(industrial_bus_protocol_compatibility
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(industrial_bus_protocol_compatibility
    PUBLIC IndustrialBus::Protocol protobuf::libprotobuf)
industrial_bus_target_defaults(industrial_bus_protocol_compatibility)

add_executable(industrial_bus_protocol_compatibility_check
    tools/protocol_compatibility_check.cpp)
target_link_libraries(industrial_bus_protocol_compatibility_check
    PRIVATE industrial_bus_protocol_compatibility)
industrial_bus_target_defaults(industrial_bus_protocol_compatibility_check)

add_executable(industrial_bus_protocol_fixture_generator
    tools/protocol_fixture_generator.cpp)
target_link_libraries(industrial_bus_protocol_fixture_generator
    PRIVATE IndustrialBus::Protocol protobuf::libprotobuf)
industrial_bus_target_defaults(industrial_bus_protocol_fixture_generator)

set(INDUSTRIAL_BUS_COMPAT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/compat/v1")
add_custom_target(industrial_bus_update_protocol_fixtures
    COMMAND industrial_bus_protocol_fixture_generator
        "${INDUSTRIAL_BUS_COMPAT_DIR}"
    DEPENDS industrial_bus_protocol_fixture_generator
    VERBATIM)
add_custom_target(industrial_bus_update_compat_baseline
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${INDUSTRIAL_BUS_DESCRIPTOR_SET}"
        "${INDUSTRIAL_BUS_COMPAT_DIR}/descriptor_set.pb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${INDUSTRIAL_BUS_CATALOG_FILE}"
        "${INDUSTRIAL_BUS_COMPAT_DIR}/topics.textproto"
    COMMAND industrial_bus_protocol_fixture_generator
        "${INDUSTRIAL_BUS_COMPAT_DIR}"
    DEPENDS industrial_bus_protocol_descriptor
            industrial_bus_protocol_fixture_generator
            "${INDUSTRIAL_BUS_CATALOG_FILE}"
    VERBATIM)

set(INDUSTRIAL_BUS_GENERATED_TOPIC_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/../api/include/industrial_bus/topics.generated.h")
set(INDUSTRIAL_BUS_GENERATED_TOPIC_TABLE
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/topic_catalog.generated.inc")
add_custom_target(industrial_bus_generate_topic_catalog
    COMMAND industrial_bus_topic_catalog_generator
        --catalog "${INDUSTRIAL_BUS_CATALOG_FILE}"
        --descriptor-set "${INDUSTRIAL_BUS_DESCRIPTOR_SET}"
        --public-out "${INDUSTRIAL_BUS_GENERATED_TOPIC_HEADER}"
        --runtime-out "${INDUSTRIAL_BUS_GENERATED_TOPIC_TABLE}"
    DEPENDS industrial_bus_topic_catalog_generator
            "${INDUSTRIAL_BUS_CATALOG_FILE}"
    VERBATIM)

if(INDUSTRIAL_BUS_BUILD_TESTS)
    add_executable(industrial_bus_topic_catalog_generator_test
        tests/topic_catalog_generator_test.cpp)
    target_link_libraries(industrial_bus_topic_catalog_generator_test
        PRIVATE industrial_bus_topic_catalog_compiler GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_topic_catalog_generator_test)
    gtest_discover_tests(industrial_bus_topic_catalog_generator_test)

    add_executable(industrial_bus_protocol_compatibility_test
        tests/compatibility_checker_test.cpp)
    target_link_libraries(industrial_bus_protocol_compatibility_test
        PRIVATE industrial_bus_protocol_compatibility GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_protocol_compatibility_test)
    gtest_discover_tests(industrial_bus_protocol_compatibility_test)

    add_executable(industrial_bus_protocol_compatibility_fixture_test
        tests/compatibility_fixture_test.cpp)
    target_link_libraries(industrial_bus_protocol_compatibility_fixture_test
        PRIVATE IndustrialBus::Protocol GTest::gtest_main)
    target_compile_definitions(industrial_bus_protocol_compatibility_fixture_test
        PRIVATE
            INDUSTRIAL_BUS_COMPAT_FIXTURE_DIR="${INDUSTRIAL_BUS_COMPAT_DIR}")
    industrial_bus_target_defaults(
        industrial_bus_protocol_compatibility_fixture_test)
    gtest_discover_tests(industrial_bus_protocol_compatibility_fixture_test)

    add_test(
        NAME ProtocolCompatibilityBaseline
        COMMAND industrial_bus_protocol_compatibility_check
            "${INDUSTRIAL_BUS_COMPAT_DIR}/descriptor_set.pb"
            "${INDUSTRIAL_BUS_DESCRIPTOR_SET}"
            "${INDUSTRIAL_BUS_COMPAT_DIR}/topics.textproto"
            "${INDUSTRIAL_BUS_CATALOG_FILE}")

    add_test(
        NAME TopicCatalogGeneratedOutputsCurrent
        COMMAND industrial_bus_topic_catalog_generator
            --catalog "${INDUSTRIAL_BUS_CATALOG_FILE}"
            --descriptor-set "${INDUSTRIAL_BUS_DESCRIPTOR_SET}"
            --public-out "${INDUSTRIAL_BUS_GENERATED_TOPIC_HEADER}"
            --runtime-out "${INDUSTRIAL_BUS_GENERATED_TOPIC_TABLE}"
            --check)
endif()
