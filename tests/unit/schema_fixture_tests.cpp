#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>

#include <array>
#include <filesystem>
#include <string_view>
#include <utility>

TEST_CASE("all JSON fixtures validate against their documented schemas") {
  const std::filesystem::path schemas = KC_TEST_SCHEMAS_DIR;
  const std::filesystem::path fixtures = KC_TEST_FIXTURES_DIR;
  constexpr std::array documents{
      std::pair<std::string_view, std::string_view>{"project-config.schema.json", "project-config.valid.json"},
      std::pair<std::string_view, std::string_view>{"extracted-page.schema.json", "extracted-page.valid.json"},
      std::pair<std::string_view, std::string_view>{"citation.schema.json", "citation.valid.json"},
      std::pair<std::string_view, std::string_view>{"article-proposal.schema.json", "article-proposal.valid.json"}};

  for (const auto& [schema_name, fixture_name] : documents) {
    CAPTURE(schema_name, fixture_name);
    nlohmann::json_schema::json_validator validator(
        [&](const nlohmann::json_uri& uri, nlohmann::json& referenced_schema) {
          referenced_schema = kc::test::read_json(schemas / std::filesystem::path(uri.path()).filename());
        });
    REQUIRE_NOTHROW(validator.set_root_schema(kc::test::read_json(schemas / schema_name)));
    REQUIRE_NOTHROW(validator.validate(kc::test::read_json(fixtures / fixture_name)));
  }
}

