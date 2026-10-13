#include "kc/vault/rendering.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("rendered diff includes additions removals context and final newline changes", "[review-preview]") {
  CHECK(kc::vault::rendered_diff("a\nkeep\nold\nz\n","a\nkeep\nnew\nz\n","vault/Topic.md") ==
        "--- a/vault/Topic.md\n+++ b/vault/Topic.md\n@@ -1,4 +1,4 @@\n a\n keep\n-old\n+new\n z\n");
  CHECK(kc::vault::rendered_diff("short","short\n","Topic.md") ==
        "--- a/Topic.md\n+++ b/Topic.md\n@@ -1,1 +1,1 @@\n-short\n\\ No newline at end of file\n+short\n");
  CHECK(kc::vault::rendered_diff("same\r\n","same\r\n","Topic.md").empty());
  CHECK(kc::vault::rendered_diff("光合成\n","光合成\nnew\n","Topic.md").find(" 光合成\n+new\n") != std::string::npos);
}

TEST_CASE("large rendered diffs preserve every removed and added line with bounded comparison memory", "[review-preview]") {
  std::string before, after;
  for (int line=0; line<2200; ++line) {
    before += "old " + std::to_string(line) + "\n";
    after += "new " + std::to_string(line) + "\n";
  }
  const auto diff = kc::vault::rendered_diff(before,after,"Topic.md");
  CHECK(diff.find("@@ -1,2200 +1,2200 @@") != std::string::npos);
  for (int line=0; line<2200; ++line) {
    CHECK(diff.find("-old " + std::to_string(line) + "\n") != std::string::npos);
    CHECK(diff.find("+new " + std::to_string(line) + "\n") != std::string::npos);
  }
}
