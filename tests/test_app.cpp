#include <gtest/gtest.h>
#include "test.hpp"

// Sample test case to verify that print_hello function works correctly
TEST(PrintHelloTest, OutputsCorrectString) {
    testing::internal::CaptureStdout();
    print_hello();
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "Hello, Market Engine!\n");
}
