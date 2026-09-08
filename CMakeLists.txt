cmake_minimum_required(VERSION 3.20)

project(
    IndustrialBus
    VERSION 0.1.0
    DESCRIPTION "Local-first communication runtime for industrial desktop tools"
    LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

set(INDUSTRIAL_BUS_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/IndustrialBus"
    CACHE STRING "Install location for IndustrialBus CMake package files")

set(industrial_bus_install_targets
    industrial_bus_core
    industrial_bus_transport_spi
    industrial_bus_transport_loopback
    industrial_bus_bootstrap
    industrial_bus_commhub_core)

foreach(optional_target IN ITEMS
        industrial_bus_protocol
        industrial_bus_transport_zeromq
        industrial_bus_client
        industrial_bus_integration
        industrial_bus_runtime
        commhub)
    if(TARGET ${optional_target})
        list(APPEND industrial_bus_install_targets ${optional_target})
    endif()
endforeach()

install(TARGETS ${industrial_bus_install_targets}
    EXPORT IndustrialBusTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY
    api/include/
    bootstrap/include/
    client/include/
    commhub/include/
    core/include/
    runtime/include/
    transport/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")

install(FILES
    transport/loopback/loopback_transport.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/industrial_bus/transport)

if(INDUSTRIAL_BUS_ENABLE_ZEROMQ)
    install(FILES
        transport/zeromq/zeromq_transport.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/industrial_bus/transport)
endif()

install(FILES
    cmake/FindZeroMQ.cmake
    cmake/Findcppzmq.cmake
    DESTINATION ${INDUSTRIAL_BUS_INSTALL_CMAKEDIR})

configure_package_config_file(
    cmake/IndustrialBusConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/IndustrialBusConfig.cmake"
    INSTALL_DESTINATION ${INDUSTRIAL_BUS_INSTALL_CMAKEDIR}
    PATH_VARS CMAKE_INSTALL_BINDIR)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/IndustrialBusConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)

install(EXPORT IndustrialBusTargets
    FILE IndustrialBusTargets.cmake
    NAMESPACE IndustrialBus::
    DESTINATION ${INDUSTRIAL_BUS_INSTALL_CMAKEDIR})

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/IndustrialBusConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/IndustrialBusConfigVersion.cmake"
    DESTINATION ${INDUSTRIAL_BUS_INSTALL_CMAKEDIR})

if(INDUSTRIAL_BUS_BUILD_TESTS AND INDUSTRIAL_BUS_ENABLE_PROTOBUF)
    configure_file(
        cmake/tests/install_consumer_test.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/install_consumer_test.cmake"
        @ONLY)
    add_test(
        NAME CMakeInstallConsumerTest
        COMMAND ${CMAKE_COMMAND}
            -DTEST_CONFIG=$<CONFIG>
            -P "${CMAKE_CURRENT_BINARY_DIR}/install_consumer_test.cmake")
endif()
