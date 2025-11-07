#include "../src/reader/reader.h"
#include <catch2/catch_test_macros.hpp>
#include <string>

// Dummy test to ensure Catch2 is properly set up
TEST_CASE("Dummy test", "[dummy]") { REQUIRE(1 + 1 == 2); }

// Test for opening an STS file
TEST_CASE("Open STS file", "[sts]") {
  const std::string sts_file_path = "tests/data/sample.sts";
  REQUIRE_NOTHROW(read_sts_file(sts_file_path));
}