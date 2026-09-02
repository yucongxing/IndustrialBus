cmake_minimum_required(VERSION 3.20)

project(
    IndustrialBus
    VERSION 0.1.0
    DESCRIPTION "Local-first communication runtime for industrial desktop tools"
    LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(INDUSTRIAL_BUS_ENABLE_PROTOBUF "Build Protobuf protocol targets" ON)
option(INDUSTRIAL_BUS_ENABLE_ZEROMQ "Build ZeroMQ transport and CommHub" ON)
option(INDUSTRIAL_BUS_BUILD_TESTS "Build IndustrialBus tests" ${BUILD_TESTING})
option(INDUSTRIAL_BUS_BUILD_SIMULATED_APPS
       "Build the non-Qt LayoutView and CMPSuite simulation programs" ON)

if(BUILD_TESTING OR INDUSTRIAL_BUS_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

if(INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    find_package(Protobuf CONFIG QUIET)
    if(NOT TARGET protobuf::libprotobuf)
        find_package(Protobuf REQUIRED)
    endif()
endif()

if(INDUSTRIAL_BUS_ENABLE_ZEROMQ)
    find_package(ZeroMQ REQUIRED)
    find_package(cppzmq REQUIRED)
endif()

function(industrial_bus_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

foreach(component IN ITEMS core protocol transport bootstrap client integration commhub runtime)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${component}/CMakeLists.txt")
        add_subdirectory(${component})
    endif()
endforeach()

if(INDUSTRIAL_BUS_BUILD_SIMULATED_APPS)
    if(INDUSTRIAL_BUS_ENABLE_PROTOBUF AND INDUSTRIAL_BUS_ENABLE_ZEROMQ)
        add_subdirectory(examples/simulated_apps)
    else()
        message(STATUS
            "Skipping simulated apps because Protobuf or ZeroMQ is disabled")
    endif()
endif()
