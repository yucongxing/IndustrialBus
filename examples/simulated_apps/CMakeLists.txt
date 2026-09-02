add_executable(layoutview_sim layoutview_sim.cpp)
target_link_libraries(layoutview_sim
    PRIVATE IndustrialBus::Client IndustrialBus::Protocol)
target_compile_definitions(layoutview_sim
    PRIVATE INDUSTRIAL_BUS_SIM_COMMHUB_PATH="$<TARGET_FILE:commhub>")
industrial_bus_target_defaults(layoutview_sim)
add_dependencies(layoutview_sim commhub)

add_executable(cmpsuite_sim cmpsuite_sim.cpp)
target_link_libraries(cmpsuite_sim
    PRIVATE IndustrialBus::Client IndustrialBus::Protocol IndustrialBus::Core)
target_compile_definitions(cmpsuite_sim
    PRIVATE INDUSTRIAL_BUS_SIM_COMMHUB_PATH="$<TARGET_FILE:commhub>")
industrial_bus_target_defaults(cmpsuite_sim)
add_dependencies(cmpsuite_sim commhub)
