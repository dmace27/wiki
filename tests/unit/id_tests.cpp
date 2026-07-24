#include "kc/domain/id.hpp"

#include <catch2/catch_test_macros.hpp>

#include <regex>

// IDs expose their object type in a prefix and use the restricted Crockford
// alphabet; two sequential calls must still produce distinct values.
TEST_CASE("generated IDs are type-prefixed Crockford ULIDs") {
  const auto first = kc::domain::generate_source_id();
  const auto second = kc::domain::generate_source_id();

  CHECK(std::regex_match(first.value, std::regex("^src_[0-9A-HJKMNP-TV-Z]{26}$")));
  CHECK(std::regex_match(kc::domain::generate_project_id().value,
                         std::regex("^prj_[0-9A-HJKMNP-TV-Z]{26}$")));
  CHECK(first.value != second.value);
}
