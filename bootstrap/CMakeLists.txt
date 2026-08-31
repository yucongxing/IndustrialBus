add_library(industrial_bus_bootstrap STATIC
    src/endpoint_record.cpp
    src/hub_lock.cpp
    src/runtime_paths.cpp
    src/secure_files.cpp
)
add_library(IndustrialBus::Bootstrap ALIAS industrial_bus_bootstrap)

target_compile_features(industrial_bus_bootstrap PUBLIC cxx_std_17)
target_include_directories(industrial_bus_bootstrap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(industrial_bus_bootstrap PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(COMMAND industrial_bus_target_defaults)
    industrial_bus_target_defaults(industrial_bus_bootstrap)
endif()

if(BUILD_TESTING)
    add_executable(industrial_bus_bootstrap_test tests/bootstrap_test.cpp)
    target_link_libraries(industrial_bus_bootstrap_test PRIVATE IndustrialBus::Bootstrap)
    add_test(NAME industrial_bus_bootstrap_test COMMAND industrial_bus_bootstrap_test)
endif()
