#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace kc::models {

/// Complete, provider-neutral input for one schema-constrained generation.
///
/// This type intentionally contains no filesystem paths or write callbacks.
/// A provider therefore cannot modify a vault, project source, or cache file.
struct LanguageModelRequest {
  std::string system_prompt;
  std::string user_prompt;
  nlohmann::json response_schema;
};

/// A model's untrusted textual response before structural validation.
struct LanguageModelResponse {
  std::string content;
};

/// Failure to contact a provider or understand its HTTP response envelope.
class ModelAdapterError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Filesystem-isolated interface implemented by local or future API models.
class LanguageModel {
 public:
  virtual ~LanguageModel() = default;

  /// Stable names persisted in model-run audit records.
  [[nodiscard]] virtual std::string_view provider_name() const noexcept = 0;
  [[nodiscard]] virtual std::string_view model_name() const noexcept = 0;

  /// Generate one untrusted response. Validation is performed by ModelRunner,
  /// never by assuming the provider honored the requested JSON schema.
  [[nodiscard]] virtual LanguageModelResponse generate(
      const LanguageModelRequest& request) = 0;
};

}  // namespace kc::models
