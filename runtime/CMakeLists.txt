if(NOT INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    return()
endif()
add_library(industrial_bus_runtime STATIC
    src/commhub_server.cpp
    src/envelope_codec.cpp
    src/transport_runtime_adapter.cpp
)
add_library(IndustrialBus::Runtime ALIAS industrial_bus_runtime)

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
        PRIVATE IndustrialBus::Runtime IndustrialBus::TransportLoopback)
    industrial_bus_target_defaults(industrial_bus_runtime_e2e_test)
    add_test(NAME industrial_bus_runtime_e2e_test COMMAND industrial_bus_runtime_e2e_test)
endif()
