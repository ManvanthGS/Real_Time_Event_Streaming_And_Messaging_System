cmake_minimum_required(VERSION 3.20)
project(sample_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(sample_project STATIC
    src/test.cpp
)

target_include_directories(sample_project
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_subdirectory(tests)
add_subdirectory(benchmarks)
add_subdirectory(examples)
