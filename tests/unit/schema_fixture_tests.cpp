#include "../test_support.hpp"
#include "../contract_cases.hpp"
#include "kc/domain/schema.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>

#include <array>
#include <filesystem>
#include <string_view>
#include <utility>

// Validate fixtures independently of the C++ parser so the checked-in JSON
// Schemas remain an accurate external contract.
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

TEST_CASE("common contract cases agree across public schemas and runtime parsing") {
  const std::filesystem::path schemas = KC_TEST_SCHEMAS_DIR;
  for (const auto& fixture : kc::test::contract_cases()) {
    CAPTURE(fixture.at("name"));
    const auto contract = kc::test::case_contract(fixture);
    const auto document = kc::test::case_document(fixture);
    // Load disk schemas independently of the production compiled registry.
    nlohmann::json_schema::json_validator validator(
        [&](const nlohmann::json_uri& uri, nlohmann::json& referenced) {
          referenced = kc::test::read_json(schemas / std::filesystem::path(uri.path()).filename());
        });
    validator.set_root_schema(kc::test::read_json(
        schemas / (kc::test::contract_basename(contract) + ".schema.json")));
    bool schema_valid = true;
    try {
      static_cast<void>(validator.validate(document));
    } catch (const std::exception&) {
      schema_valid = false;
    }
    CHECK(schema_valid == fixture.at("schema_valid").get<bool>());
    CHECK(kc::domain::validate_json_shape(contract, document).valid() == schema_valid);
    CHECK(kc::test::parses_contract(contract, document) ==
          fixture.at("runtime_valid").get<bool>());
  }
}

TEST_CASE("compiled and model schemas retain every public schema constraint") {
  const std::filesystem::path schemas = KC_TEST_SCHEMAS_DIR;
  for (const auto contract : {kc::domain::JsonContract::project_config,
                             kc::domain::JsonContract::extracted_page,
                             kc::domain::JsonContract::citation}) {
    CHECK(kc::domain::contract_schema(contract) == kc::test::read_json(
        schemas / (kc::test::contract_basename(contract) + ".schema.json")));
  }
  auto expected = kc::test::read_json(schemas / "article-proposal.schema.json");
  auto citation = kc::test::read_json(schemas / "citation.schema.json");
  citation.erase("$id");
  expected["$defs"]["citation"] = citation;
  expected["properties"]["sections"]["items"]["properties"]["blocks"]["items"]
      ["properties"]["citations"]["items"]["$ref"] = "#/$defs/citation";
  expected["properties"]["related_concepts"]["items"]["properties"]
      ["citations"]["items"]["$ref"] = "#/$defs/citation";
  CHECK(kc::domain::contract_schema(kc::domain::JsonContract::article_proposal) == expected);
}
