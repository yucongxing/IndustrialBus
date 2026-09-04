if(NOT TARGET IndustrialBus::Client)
    return()
endif()

add_library(industrial_bus_integration STATIC
    operation_guard/operation.cpp
    progress/progress_throttle.cpp
)
add_library(IndustrialBus::Integration ALIAS industrial_bus_integration)
set_target_properties(industrial_bus_integration PROPERTIES EXPORT_NAME Integration)

target_include_directories(industrial_bus_integration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../api/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../client/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(industrial_bus_integration PUBLIC IndustrialBus::Client)
industrial_bus_target_defaults(industrial_bus_integration)
