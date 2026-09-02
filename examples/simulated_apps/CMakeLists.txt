add_library(simulated_logger STATIC simple_logger.cpp)
target_include_directories(simulated_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
industrial_bus_target_defaults(simulated_logger)

add_library(simulated_app_support STATIC bus_simulation_adapter.cpp)
target_include_directories(simulated_app_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simulated_app_support
    PUBLIC simulated_logger
    PRIVATE IndustrialBus::Client IndustrialBus::Protocol IndustrialBus::Core)
target_compile_definitions(simulated_app_support
    PRIVATE INDUSTRIAL_BUS_SIM_COMMHUB_PATH="$<TARGET_FILE:commhub>")
industrial_bus_target_defaults(simulated_app_support)
add_dependencies(simulated_app_support commhub)

add_executable(layoutview_sim layoutview_sim.cpp)
target_link_libraries(layoutview_sim PRIVATE simulated_app_support)
industrial_bus_target_defaults(layoutview_sim)

add_executable(cmpsuite_sim cmpsuite_sim.cpp)
target_link_libraries(cmpsuite_sim PRIVATE simulated_app_support)
industrial_bus_target_defaults(cmpsuite_sim)

if(INDUSTRIAL_BUS_BUILD_TESTS)
    add_executable(simulated_logger_test simple_logger_test.cpp)
    target_link_libraries(simulated_logger_test
        PRIVATE simulated_logger GTest::gtest_main)
    industrial_bus_target_defaults(simulated_logger_test)
    gtest_discover_tests(simulated_logger_test)

    add_executable(simulated_channel_test simulation_channel_test.cpp)
    target_link_libraries(simulated_channel_test
        PRIVATE simulated_app_support IndustrialBus::Protocol IndustrialBus::Core
                GTest::gtest_main)
    industrial_bus_target_defaults(simulated_channel_test)
    gtest_discover_tests(simulated_channel_test)
endif()
