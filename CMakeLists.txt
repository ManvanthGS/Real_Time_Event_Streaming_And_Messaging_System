cmake_minimum_required(VERSION 3.20)
project(messaging_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Core library: the foundation that all binaries depend on.
# ---------------------------------------------------------------------------
add_library(messaging_core STATIC
    src/network/socket.cpp
    src/protocol/framing.cpp
    src/broker/broker.cpp
)

target_include_directories(messaging_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(messaging_core PUBLIC ws2_32)
else()
    target_link_libraries(messaging_core PUBLIC pthread)
endif()

# Dummy test library to keep tests compiling for now
add_library(messaging_system STATIC src/test.cpp)
target_include_directories(messaging_system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Main application binaries (Phase 1)
# ---------------------------------------------------------------------------
add_executable(broker src/app/broker_main.cpp)
target_link_libraries(broker PRIVATE messaging_core)

add_executable(producer src/app/producer_main.cpp)
target_link_libraries(producer PRIVATE messaging_core)

add_executable(consumer src/app/consumer_main.cpp)
target_link_libraries(consumer PRIVATE messaging_core)

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
