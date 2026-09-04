add_library(industrial_bus_transport_spi INTERFACE)
add_library(IndustrialBus::TransportSPI ALIAS industrial_bus_transport_spi)
set_target_properties(industrial_bus_transport_spi
    PROPERTIES EXPORT_NAME TransportSPI)
target_include_directories(industrial_bus_transport_spi
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

add_library(industrial_bus_transport_loopback STATIC
    loopback/loopback_transport.cpp
)
add_library(IndustrialBus::TransportLoopback ALIAS industrial_bus_transport_loopback)
set_target_properties(industrial_bus_transport_loopback
    PROPERTIES EXPORT_NAME TransportLoopback)
target_compile_features(industrial_bus_transport_loopback PUBLIC cxx_std_17)
target_include_directories(industrial_bus_transport_loopback
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/loopback>
            $<INSTALL_INTERFACE:include>
            $<INSTALL_INTERFACE:include/industrial_bus/transport>
)
set_target_properties(industrial_bus_transport_loopback PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(industrial_bus_transport_loopback
    PUBLIC IndustrialBus::TransportSPI)

if(INDUSTRIAL_BUS_ENABLE_ZEROMQ)
    add_library(industrial_bus_transport_zeromq STATIC
        zeromq/zeromq_transport.cpp
    )
    add_library(IndustrialBus::TransportZeroMQ ALIAS industrial_bus_transport_zeromq)
    set_target_properties(industrial_bus_transport_zeromq
        PROPERTIES EXPORT_NAME TransportZeroMQ)
    target_compile_features(industrial_bus_transport_zeromq PUBLIC cxx_std_17)
    target_include_directories(industrial_bus_transport_zeromq
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/zeromq>
            $<INSTALL_INTERFACE:include>
            $<INSTALL_INTERFACE:include/industrial_bus/transport>
    )
    target_link_libraries(industrial_bus_transport_zeromq
        PUBLIC
            ZeroMQ::libzmq
            cppzmq::cppzmq
    )
    set_target_properties(industrial_bus_transport_zeromq PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(industrial_bus_transport_zeromq
        PUBLIC IndustrialBus::TransportSPI)

    if(BUILD_TESTING)
        add_executable(industrial_bus_zeromq_transport_test
            tests/zeromq_transport_test.cpp
        )
        target_link_libraries(industrial_bus_zeromq_transport_test
            PRIVATE IndustrialBus::TransportZeroMQ GTest::gtest_main
        )
        gtest_discover_tests(industrial_bus_zeromq_transport_test
            PROPERTIES TIMEOUT 5)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(industrial_bus_loopback_transport_test
        tests/loopback_transport_test.cpp
    )
    target_link_libraries(industrial_bus_loopback_transport_test
        PRIVATE IndustrialBus::TransportLoopback GTest::gtest_main
    )
    gtest_discover_tests(industrial_bus_loopback_transport_test
        PROPERTIES TIMEOUT 5)
endif()
