if(NOT INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    return()
endif()

add_library(industrial_bus_client STATIC
    src/bus_client.cpp
    src/default_executor.cpp
    src/pending_request_table.cpp
)
add_library(IndustrialBus::Client ALIAS industrial_bus_client)

target_include_directories(industrial_bus_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../api/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(industrial_bus_client
    PUBLIC IndustrialBus::Core IndustrialBus::Protocol Threads::Threads)
industrial_bus_target_defaults(industrial_bus_client)

if(INDUSTRIAL_BUS_BUILD_TESTS)
    add_executable(industrial_bus_client_tests
        tests/client_runtime_test.cpp
    )
    target_link_libraries(industrial_bus_client_tests PRIVATE IndustrialBus::Client)
    industrial_bus_target_defaults(industrial_bus_client_tests)
    add_test(NAME industrial_bus_client_tests COMMAND industrial_bus_client_tests)
endif()
