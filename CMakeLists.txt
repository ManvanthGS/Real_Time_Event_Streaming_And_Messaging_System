cmake_minimum_required(VERSION 3.20)
project(messaging_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Core library: only the placeholder test module for now.
# The actual networking is compiled into messaging_app to keep the
# build simple until we have a proper unit-testable interface (Phase 1+).
# ---------------------------------------------------------------------------
add_library(messaging_system STATIC
    src/test.cpp
)

target_include_directories(messaging_system
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Main application sources.
# Phase 0: Added src/protocol/framing.cpp which implements the framing layer.
# ---------------------------------------------------------------------------
set(SOURCES
    src/app/main.cpp
    src/network/socket.cpp
    src/network/messaging_node.cpp
    src/protocol/framing.cpp
)

add_executable(messaging_app ${SOURCES})

target_include_directories(messaging_app
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Platform-specific linker flags
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(messaging_app ws2_32)
endif()

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
