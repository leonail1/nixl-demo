cmake_minimum_required(VERSION 3.16)
project(nixl_ucx_demo LANGUAGES CXX)

# Allow callers to override the NIXL install prefix.
set(NIXL_ROOT "/opt/nvidia/nvda_nixl" CACHE PATH "Path to the NIXL installation prefix")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckIncludeFileCXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

set(NIXL_INCLUDE_DIR "${NIXL_ROOT}/include" CACHE PATH "NIXL include directory")

set(_nixl_default_lib "${NIXL_ROOT}/lib")
if(EXISTS "${NIXL_ROOT}/lib/x86_64-linux-gnu")
    set(_nixl_default_lib "${NIXL_ROOT}/lib/x86_64-linux-gnu")
endif()
set(NIXL_LIBRARY_DIR "${_nixl_default_lib}" CACHE PATH "NIXL library directory")

if(NOT EXISTS "${NIXL_INCLUDE_DIR}/nixl.h")
    message(FATAL_ERROR "Failed to locate nixl.h under ${NIXL_INCLUDE_DIR}. Set NIXL_INCLUDE_DIR appropriately.")
endif()

if(NOT EXISTS "${NIXL_LIBRARY_DIR}/libnixl.so" AND NOT EXISTS "${NIXL_LIBRARY_DIR}/libnixl.a")
    message(FATAL_ERROR "Failed to locate libnixl.[so|a] under ${NIXL_LIBRARY_DIR}. Set NIXL_LIBRARY_DIR appropriately.")
endif()

function(_nixl_register_shared name)
    set(options)
    set(oneValueArgs LOCATION)
    cmake_parse_arguments(_nixl "${options}" "${oneValueArgs}" "" ${ARGN})
    if(NOT _nixl_LOCATION)
        set(_nixl_LOCATION "${NIXL_LIBRARY_DIR}/lib${name}.so")
    endif()
    if(NOT EXISTS "${_nixl_LOCATION}")
        message(FATAL_ERROR "Required NIXL library ${_nixl_LOCATION} not found. Adjust NIXL_LIBRARY_DIR.")
    endif()
    add_library(nixl::${name} SHARED IMPORTED)
    set_target_properties(nixl::${name} PROPERTIES
        IMPORTED_LOCATION "${_nixl_LOCATION}"
        INTERFACE_INCLUDE_DIRECTORIES "${NIXL_INCLUDE_DIR}"
        INTERFACE_LINK_DIRECTORIES "${NIXL_LIBRARY_DIR}"
    )
endfunction()

_nixl_register_shared(nixl)
_nixl_register_shared(nixl_build)
_nixl_register_shared(nixl_common)
_nixl_register_shared(stream)
_nixl_register_shared(serdes)
_nixl_register_shared(file_utils)
_nixl_register_shared(ucx_utils)

function(add_nixl_demo executable)
    add_executable(${executable} ${ARGN})
    target_include_directories(${executable}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${NIXL_INCLUDE_DIR}
            ${NIXL_INCLUDE_DIR}/utils
    )
    target_link_libraries(${executable}
        PRIVATE
            nixl::nixl
            nixl::nixl_build
            nixl::nixl_common
            nixl::stream
            nixl::serdes
            nixl::file_utils
            nixl::ucx_utils
            pthread
    )
    set_property(TARGET ${executable} PROPERTY BUILD_RPATH "${NIXL_LIBRARY_DIR}")
    set_property(TARGET ${executable} PROPERTY INSTALL_RPATH "${NIXL_LIBRARY_DIR}")
endfunction()

add_nixl_demo(nixl_demo_server server.cpp util.h)
add_nixl_demo(nixl_demo_client client.cpp util.h)

install(TARGETS nixl_demo_server nixl_demo_client
        RUNTIME DESTINATION bin)
