cmake_minimum_required(VERSION 3.20)
project(messaging_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(messaging_system STATIC
    src/test.cpp
)

add_executable(client src/network/client.cpp)
add_executable(server src/network/server.cpp)

target_include_directories(client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(server
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
    target_link_libraries(client ws2_32)
    target_link_libraries(server ws2_32)
endif()
