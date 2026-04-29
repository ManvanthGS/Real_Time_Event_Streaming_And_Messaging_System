include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(messaging_system_tests
    test_app.cpp
)

target_link_libraries(messaging_system_tests
    PRIVATE
        messaging_system
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(messaging_system_tests)
