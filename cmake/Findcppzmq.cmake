find_path(cppzmq_INCLUDE_DIR NAMES zmq.hpp)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(cppzmq REQUIRED_VARS cppzmq_INCLUDE_DIR)

if(cppzmq_FOUND AND NOT TARGET cppzmq)
    add_library(cppzmq INTERFACE IMPORTED)
    set_target_properties(
        cppzmq
        PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${cppzmq_INCLUDE_DIR}")
endif()

if(cppzmq_FOUND AND NOT TARGET cppzmq::cppzmq)
    add_library(cppzmq::cppzmq ALIAS cppzmq)
endif()

mark_as_advanced(cppzmq_INCLUDE_DIR)
