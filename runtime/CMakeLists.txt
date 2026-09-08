if(NOT INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    return()
endif()
add_library(industrial_bus_runtime STATIC
    src/commhub_server.cpp
)
add_library(IndustrialBus::Runtime ALIAS industrial_bus_runtime)
set_target_properties(industrial_bus_runtime PROPERTIES EXPORT_NAME Runtime)

target_include_directories(industrial_bus_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(industrial_bus_runtime
    PUBLIC
        IndustrialBus::Client
        IndustrialBus::CommHubCore
        IndustrialBus::Protocol
        IndustrialBus::TransportLoopback
)
industrial_bus_target_defaults(industrial_bus_runtime)

if(INDUSTRIAL_BUS_BUILD_TESTS)
    add_executable(industrial_bus_runtime_e2e_test tests/runtime_e2e_test.cpp)
    target_link_libraries(industrial_bus_runtime_e2e_test
        PRIVATE IndustrialBus::Runtime IndustrialBus::TransportLoopback
                GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_runtime_e2e_test)
    gtest_discover_tests(industrial_bus_runtime_e2e_test)

    add_executable(industrial_bus_runtime_heartbeat_test
        tests/heartbeat_test.cpp)
    target_link_libraries(industrial_bus_runtime_heartbeat_test
        PRIVATE IndustrialBus::Runtime IndustrialBus::TransportLoopback
                GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_runtime_heartbeat_test)
    gtest_discover_tests(industrial_bus_runtime_heartbeat_test
        PROPERTIES TIMEOUT 5)

    add_executable(industrial_bus_protocol_validation_test
        tests/protocol_validation_test.cpp)
    target_link_libraries(industrial_bus_protocol_validation_test
        PRIVATE IndustrialBus::Runtime IndustrialBus::TransportLoopback
                GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_protocol_validation_test)
    gtest_discover_tests(industrial_bus_protocol_validation_test)

    add_executable(industrial_bus_topic_catalog_test
        tests/topic_catalog_test.cpp)
    target_link_libraries(industrial_bus_topic_catalog_test
        PRIVATE IndustrialBus::Runtime GTest::gtest_main)
    industrial_bus_target_defaults(industrial_bus_topic_catalog_test)
    gtest_discover_tests(industrial_bus_topic_catalog_test)

    add_executable(industrial_bus_protocol_validation_e2e_test
        tests/protocol_validation_e2e_test.cpp)
    target_link_libraries(industrial_bus_protocol_validation_e2e_test
        PRIVATE IndustrialBus::Runtime IndustrialBus::TransportLoopback
                GTest::gtest_main)
    industrial_bus_target_defaults(
        industrial_bus_protocol_validation_e2e_test)
    gtest_discover_tests(industrial_bus_protocol_validation_e2e_test)
endif()

if(INDUSTRIAL_BUS_ENABLE_ZEROMQ)
    add_executable(commhub ../commhub/main.cpp)
    set_target_properties(commhub PROPERTIES EXPORT_NAME CommHub)
    target_link_libraries(commhub
        PRIVATE
            IndustrialBus::Runtime
            IndustrialBus::Bootstrap
            IndustrialBus::TransportZeroMQ)
    industrial_bus_target_defaults(commhub)

    if(INDUSTRIAL_BUS_BUILD_TESTS)
        add_executable(industrial_bus_linux_client_start_test
            tests/linux_client_start_test.cpp)
        target_link_libraries(industrial_bus_linux_client_start_test
            PRIVATE IndustrialBus::Client IndustrialBus::Bootstrap
                    IndustrialBus::TransportZeroMQ GTest::gtest_main)
        target_compile_definitions(industrial_bus_linux_client_start_test
            PRIVATE
                INDUSTRIAL_BUS_TEST_COMMHUB_PATH="$<TARGET_FILE:commhub>")
        add_dependencies(industrial_bus_linux_client_start_test commhub)
        industrial_bus_target_defaults(industrial_bus_linux_client_start_test)
        gtest_discover_tests(industrial_bus_linux_client_start_test
            PROPERTIES TIMEOUT 15)
    endif()
endif()
