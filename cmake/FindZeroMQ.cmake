find_path(ZeroMQ_INCLUDE_DIR NAMES zmq.h)
find_library(ZeroMQ_LIBRARY NAMES zmq libzmq)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    ZeroMQ
    REQUIRED_VARS ZeroMQ_INCLUDE_DIR ZeroMQ_LIBRARY)

if(ZeroMQ_FOUND AND NOT TARGET ZeroMQ::libzmq)
    add_library(ZeroMQ::libzmq UNKNOWN IMPORTED)
    set_target_properties(
        ZeroMQ::libzmq
        PROPERTIES
            IMPORTED_LOCATION "${ZeroMQ_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZeroMQ_INCLUDE_DIR}")
endif()

mark_as_advanced(ZeroMQ_INCLUDE_DIR ZeroMQ_LIBRARY)
