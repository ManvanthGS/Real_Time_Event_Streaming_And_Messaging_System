include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(sample_project_tests
    test_app.cpp
)

target_link_libraries(sample_project_tests
    PRIVATE
        sample_project
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(sample_project_tests)
