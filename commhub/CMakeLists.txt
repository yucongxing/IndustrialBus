add_library(industrial_bus_commhub_core STATIC
    src/hub_core.cpp
    src/message_router.cpp
    src/qos_scheduler.cpp
    src/return_route_table.cpp
    src/service_registry.cpp
    src/session_registry.cpp
)
add_library(IndustrialBus::CommHubCore ALIAS industrial_bus_commhub_core)
target_compile_features(industrial_bus_commhub_core PUBLIC cxx_std_17)
target_include_directories(industrial_bus_commhub_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(industrial_bus_commhub_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(industrial_bus_commhub_router_test tests/router_test.cpp)
    target_link_libraries(industrial_bus_commhub_router_test
        PRIVATE IndustrialBus::CommHubCore)
    add_test(NAME industrial_bus_commhub_router_test
             COMMAND industrial_bus_commhub_router_test)

    add_executable(industrial_bus_commhub_qos_test tests/qos_scheduler_test.cpp)
    target_link_libraries(industrial_bus_commhub_qos_test
        PRIVATE IndustrialBus::CommHubCore)
    add_test(NAME industrial_bus_commhub_qos_test
             COMMAND industrial_bus_commhub_qos_test)
endif()
