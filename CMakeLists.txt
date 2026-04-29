cmake_minimum_required(VERSION 3.20)
project(messaging_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(messaging_system STATIC
    src/test.cpp
)

set(SOURCES
    src/app/main.cpp
    src/network/socket.cpp
    src/network/messaging_node.cpp
)

add_executable(messaging_app ${SOURCES})

target_include_directories(messaging_app
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(messaging_system
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)

if(WIN32)
    target_link_libraries(messaging_app ws2_32)
endif()
