add_library(industrial_bus_core STATIC
    src/idempotency_journal.cpp
    src/ids.cpp
    src/operation_state.cpp
)
add_library(IndustrialBus::Core ALIAS industrial_bus_core)

target_compile_features(industrial_bus_core PUBLIC cxx_std_17)
target_include_directories(industrial_bus_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../api/include>
)

if(BUILD_TESTING)
    add_executable(industrial_bus_core_tests tests/core_tests.cpp)
    target_link_libraries(industrial_bus_core_tests PRIVATE IndustrialBus::Core)
    add_test(NAME industrial_bus_core_tests COMMAND industrial_bus_core_tests)
endif()
