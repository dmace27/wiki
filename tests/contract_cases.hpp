#pragma once

#include "test_support.hpp"
#include "kc/domain/json.hpp"
#include "kc/domain/schema.hpp"

#include <stdexcept>

namespace kc::test {

inline domain::JsonContract case_contract(const nlohmann::json& fixture) {
  const auto name = fixture.at("contract").get<std::string>();
  if (name == "project_config") return domain::JsonContract::project_config;
  if (name == "extracted_page") return domain::JsonContract::extracted_page;
  if (name == "citation") return domain::JsonContract::citation;
  if (name == "article_proposal") return domain::JsonContract::article_proposal;
  throw std::runtime_error("unknown contract fixture: " + name);
}

inline std::string contract_basename(const domain::JsonContract contract) {
  switch (contract) {
    case domain::JsonContract::project_config: return "project-config";
    case domain::JsonContract::extracted_page: return "extracted-page";
    case domain::JsonContract::citation: return "citation";
    case domain::JsonContract::article_proposal: return "article-proposal";
  }
  throw std::runtime_error("unknown JSON contract");
}

/// A single checked-in corpus is used by independent schema validation,
/// runtime parsing, model generation, review, approval, and apply regressions.
inline nlohmann::json contract_cases() {
  return read_json(std::filesystem::path(KC_TEST_FIXTURES_DIR) / "contracts/cases.json");
}

inline nlohmann::json case_document(const nlohmann::json& fixture) {
  const auto base = read_json(std::filesystem::path(KC_TEST_FIXTURES_DIR) /
      (contract_basename(case_contract(fixture)) + ".valid.json"));
  return base.patch(fixture.at("patches"));
}

inline bool parses_contract(const domain::JsonContract contract,
                            const nlohmann::json& document) {
  switch (contract) {
    case domain::JsonContract::project_config:
      return domain::parse_project_config(document).valid();
    case domain::JsonContract::extracted_page:
      return domain::parse_extracted_page(document).valid();
    case domain::JsonContract::citation:
      return domain::parse_citation(document).valid();
    case domain::JsonContract::article_proposal:
      return domain::parse_article_proposal(document).valid();
  }
  return false;
}

}  // namespace kc::test
