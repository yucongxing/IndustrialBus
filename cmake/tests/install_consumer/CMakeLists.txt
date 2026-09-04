cmake_minimum_required(VERSION 3.20)

project(IndustrialBusInstallConsumer LANGUAGES CXX)

find_package(IndustrialBus 0.1 CONFIG REQUIRED
    COMPONENTS Client Protocol Core CommHub)

foreach(required_target IN ITEMS
        IndustrialBus::Core
        IndustrialBus::Protocol
        IndustrialBus::Client
        IndustrialBus::CommHub)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Installed package is missing ${required_target}")
    endif()
endforeach()

if(NOT EXISTS "${IndustrialBus_COMMHUB_EXECUTABLE}")
    message(FATAL_ERROR
        "Installed CommHub was not found: ${IndustrialBus_COMMHUB_EXECUTABLE}")
endif()

add_executable(industrial_bus_install_consumer main.cpp)
target_compile_features(industrial_bus_install_consumer PRIVATE cxx_std_17)
target_link_libraries(industrial_bus_install_consumer PRIVATE
    IndustrialBus::Client
    IndustrialBus::Protocol
    IndustrialBus::Core)
