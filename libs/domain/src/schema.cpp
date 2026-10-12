#include "kc/domain/schema.hpp"

#include "schema_documents.hpp"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace kc::domain {
namespace {

using Json = nlohmann::json;

/// Resolve the one external reference in the public proposal schema. Both
/// standalone validation and Ollama receive the same constraints.
void embed_citation(Json& node) {
  if (node.is_object()) {
    if (node.value("$ref", "") == "citation.schema.json") {
      node["$ref"] = "#/$defs/citation";
    }
    for (auto& child : node) {
      embed_citation(child);
    }
  } else if (node.is_array()) {
    for (auto& child : node) {
      embed_citation(child);
    }
  }
}

class IssueCollector final : public nlohmann::json_schema::error_handler {
public:
  ValidationResult result;

  void error(const Json::json_pointer& pointer, const Json&,
             const std::string& message) override {
    result.issues.push_back({pointer.to_string(), message});
  }
};

/// The schema library stores numeric comparisons as signed integers. Reject
/// wrong primitive types and out-of-range unsigned values BEFORE invoking it,
/// preventing either schema evaluation or C++ conversion from wrapping them.
void check_integer_tokens(const Json& schema, const Json& root,
                           const Json& document, const Json::json_pointer& path,
                           ValidationResult& result) {
  if (schema.contains("$ref")) {
    const auto reference = schema.at("$ref").get<std::string>();
    check_integer_tokens(root.at(Json::json_pointer(reference.substr(1))),
                         root, document, path, result);
    return;
  }
  if (schema.value("type", "") == "integer") {
    if (!document.is_number_integer() ||
        (!document.is_number_unsigned() && document.get<std::int64_t>() < 0)) {
      result.issues.push_back({path.to_string(), "must be a nonnegative JSON integer token (no floats, booleans, or strings)"});
      return;
    }
    const auto value = document.is_number_unsigned()
        ? document.get<std::uint64_t>()
        : static_cast<std::uint64_t>(document.get<std::int64_t>());
    const auto minimum = schema.value("minimum", std::uint64_t{0});
    const auto maximum = schema.value("maximum", max_citation_end);
    if (value < minimum || value > maximum) {
      result.issues.push_back({path.to_string(), "integer is outside the supported range"});
    }
  }
  if (document.is_object() && schema.contains("properties")) {
    for (const auto& [key, child] : schema.at("properties").items()) {
      if (document.contains(key)) {
        check_integer_tokens(child, root, document.at(key), path / key, result);
      }
    }
  }
  if (document.is_array() && schema.contains("items")) {
    for (std::size_t index = 0; index < document.size(); ++index) {
      check_integer_tokens(schema.at("items"), root, document[index],
                           path / std::to_string(index), result);
    }
  }
}

}  // namespace

const Json& contract_schema(const JsonContract contract) {
  static const std::array schemas = [] {
    auto proposal = Json::parse(detail::proposal_schema);
    embed_citation(proposal);
    auto citation = Json::parse(detail::citation_schema);
    citation.erase("$id");
    proposal["$defs"]["citation"] = citation;
    return std::array{Json::parse(detail::config_schema),
                      Json::parse(detail::page_schema),
                      Json::parse(detail::citation_schema), std::move(proposal)};
  }();
  return schemas.at(static_cast<std::size_t>(contract));
}

ValidationResult validate_json_shape(const JsonContract contract,
                                     const Json& document) {
  const auto& schema = contract_schema(contract);
  ValidationResult tokens;
  check_integer_tokens(schema, schema, document, Json::json_pointer{}, tokens);
  if (!tokens) {
    return tokens;
  }
  // Validators are immutable after construction. Keep one per thread so
  // validation remains independent even if the dependency changes internally.
  thread_local const std::array validators{
      nlohmann::json_schema::json_validator(contract_schema(JsonContract::project_config)),
      nlohmann::json_schema::json_validator(contract_schema(JsonContract::extracted_page)),
      nlohmann::json_schema::json_validator(contract_schema(JsonContract::citation)),
      nlohmann::json_schema::json_validator(contract_schema(JsonContract::article_proposal))};
  IssueCollector collector;
  static_cast<void>(validators.at(static_cast<std::size_t>(contract)).validate(document, collector));
  return collector.result;
}

}  // namespace kc::domain
