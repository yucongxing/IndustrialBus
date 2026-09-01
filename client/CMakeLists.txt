if(NOT INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    return()
endif()

add_library(industrial_bus_client STATIC
    ../runtime/src/envelope_codec.cpp
    ../runtime/src/transport_runtime_adapter.cpp
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
target_include_directories(industrial_bus_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../commhub/include
)
target_link_libraries(industrial_bus_client
    PUBLIC IndustrialBus::Core IndustrialBus::Protocol Threads::Threads
    PRIVATE IndustrialBus::Bootstrap IndustrialBus::TransportSPI)

if(INDUSTRIAL_BUS_ENABLE_ZEROMQ)
    target_sources(industrial_bus_client PRIVATE src/linux_runtime_adapter.cpp)
    target_link_libraries(industrial_bus_client PRIVATE IndustrialBus::TransportZeroMQ)
    target_compile_definitions(industrial_bus_client
        PRIVATE INDUSTRIAL_BUS_HAS_LINUX_RUNTIME=1)
endif()
industrial_bus_target_defaults(industrial_bus_client)

if(INDUSTRIAL_BUS_BUILD_TESTS)
    add_executable(industrial_bus_client_tests
        tests/client_runtime_test.cpp
    )
    target_link_libraries(industrial_bus_client_tests PRIVATE IndustrialBus::Client)
    industrial_bus_target_defaults(industrial_bus_client_tests)
    add_test(NAME industrial_bus_client_tests COMMAND industrial_bus_client_tests)
endif()
