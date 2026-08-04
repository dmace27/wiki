#pragma once

#include "kc/domain/types.hpp"
#include "kc/models/language_model.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace kc::models {

/// Minimal HTTP request required by an Ollama-compatible provider.
struct HttpRequest {
  std::string url;
  std::string json_body;
  std::uint32_t timeout_seconds{300};
};

/// HTTP response body plus its status line code.
struct HttpResponse {
  long status_code{0};
  std::string body;
};

class HttpError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Injectable HTTP boundary. Tests can inspect requests without running an
/// Ollama server, while production uses CurlHttpClient.
class HttpClient {
 public:
  virtual ~HttpClient() = default;
  [[nodiscard]] virtual HttpResponse post_json(const HttpRequest& request) = 0;
};

/// libcurl implementation with bounded responses and no authentication state.
class CurlHttpClient final : public HttpClient {
 public:
  [[nodiscard]] HttpResponse post_json(const HttpRequest& request) override;
};

/// Ollama `/api/chat` adapter using non-streaming JSON-schema output.
///
/// The adapter receives already-parsed settings from `kc.json`. It has an HTTP
/// capability only: it is never given the project root or a vault writer.
class OllamaLanguageModel final : public LanguageModel {
 public:
  OllamaLanguageModel(
      domain::OllamaConfig config,
      std::shared_ptr<HttpClient> http_client =
          std::make_shared<CurlHttpClient>());

  [[nodiscard]] std::string_view provider_name() const noexcept override;
  [[nodiscard]] std::string_view model_name() const noexcept override;
  [[nodiscard]] LanguageModelResponse generate(
      const LanguageModelRequest& request) override;

 private:
  domain::OllamaConfig config_;
  std::shared_ptr<HttpClient> http_client_;
};

}  // namespace kc::models
