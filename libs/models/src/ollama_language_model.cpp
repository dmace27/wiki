#include "kc/models/ollama_language_model.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace kc::models {
namespace {

std::string chat_endpoint(std::string base_url) {
  if ((!base_url.starts_with("http://") &&
       !base_url.starts_with("https://")) ||
      base_url.find('@') != std::string::npos) {
    throw ModelAdapterError(
        "Ollama base URL must be HTTP(S) and must not contain credentials");
  }
  while (base_url.ends_with('/')) {
    base_url.pop_back();
  }
  const auto authority = base_url.find("://") + 3U;
  if (authority >= base_url.size()) {
    throw ModelAdapterError("Ollama base URL must include a host");
  }
  return base_url + "/api/chat";
}

}  // namespace

OllamaLanguageModel::OllamaLanguageModel(
    domain::OllamaConfig config, std::shared_ptr<HttpClient> http_client)
    : config_(std::move(config)), http_client_(std::move(http_client)) {
  if (!http_client_) {
    throw ModelAdapterError("Ollama adapter requires an HTTP client");
  }
  if (config_.model.empty() || config_.timeout_seconds == 0U) {
    throw ModelAdapterError("Ollama model settings are incomplete");
  }
  // Validate eagerly so a malformed project URL fails before any model run.
  static_cast<void>(chat_endpoint(config_.base_url));
}

std::string_view OllamaLanguageModel::provider_name() const noexcept {
  return "ollama";
}

std::string_view OllamaLanguageModel::model_name() const noexcept {
  return config_.model;
}

LanguageModelResponse OllamaLanguageModel::generate(
    const LanguageModelRequest& request) {
  // Ollama's native chat endpoint accepts a JSON Schema in `format`. Streaming
  // is disabled because one complete JSON envelope is simpler and safer to
  // validate than a sequence of partial fragments.
  const nlohmann::json body{
      {"model", config_.model},
      {"messages",
       {{{"role", "system"}, {"content", request.system_prompt}},
        {{"role", "user"}, {"content", request.user_prompt}}}},
      {"format", request.response_schema},
      {"stream", false},
      {"options", {{"temperature", 0}}}};

  HttpResponse response;
  try {
    response = http_client_->post_json(
        {.url = chat_endpoint(config_.base_url),
         .json_body = body.dump(),
         .timeout_seconds = config_.timeout_seconds});
  } catch (const HttpError&) {
    // Do not propagate transport details, response bodies, URLs, or headers.
    throw ModelAdapterError("Ollama HTTP request failed");
  }

  if (response.status_code < 200L || response.status_code >= 300L) {
    // Ollama error bodies may contain local model or infrastructure details;
    // only the numeric status is safe for the immediate caller.
    throw ModelAdapterError(
        "Ollama returned HTTP status " +
        std::to_string(response.status_code));
  }

  try {
    const auto envelope = nlohmann::json::parse(response.body);
    if (!envelope.is_object() || !envelope.value("done", false) ||
        !envelope.contains("message") ||
        !envelope.at("message").is_object() ||
        !envelope.at("message").contains("content") ||
        !envelope.at("message").at("content").is_string()) {
      throw ModelAdapterError("Ollama returned an incomplete response envelope");
    }
    return {envelope.at("message").at("content").get<std::string>()};
  } catch (const nlohmann::json::exception&) {
    throw ModelAdapterError("Ollama returned a malformed response envelope");
  }
}

}  // namespace kc::models
