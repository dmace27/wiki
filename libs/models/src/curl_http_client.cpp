#include "kc/models/ollama_language_model.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>

namespace kc::models {
namespace {

constexpr std::size_t maximum_response_bytes = 16U * 1024U * 1024U;

struct ResponseBuffer {
  std::string body;
  bool exceeded_limit{false};
};

/// libcurl callback that enforces a hard response-size ceiling.
std::size_t append_response(char* data, const std::size_t size,
                            const std::size_t count, void* user_data) {
  auto& response = *static_cast<ResponseBuffer*>(user_data);
  if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size) {
    response.exceeded_limit = true;
    return 0U;
  }
  const auto bytes = size * count;
  if (response.body.size() > maximum_response_bytes ||
      bytes > maximum_response_bytes - response.body.size()) {
    response.exceeded_limit = true;
    return 0U;
  }
  response.body.append(data, bytes);
  return bytes;
}

/// Initialize libcurl exactly once before the first easy handle is created.
void ensure_curl_initialized() {
  static std::once_flag initialization;
  static CURLcode status = CURLE_FAILED_INIT;
  std::call_once(initialization, [] { status = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (status != CURLE_OK) {
    throw HttpError("could not initialize local HTTP transport");
  }
}

class CurlHandle {
 public:
  CurlHandle() : handle_(curl_easy_init()) {
    if (handle_ == nullptr) {
      throw HttpError("could not create local HTTP request");
    }
  }
  ~CurlHandle() { curl_easy_cleanup(handle_); }
  CurlHandle(const CurlHandle&) = delete;
  CurlHandle& operator=(const CurlHandle&) = delete;

  [[nodiscard]] CURL* get() const noexcept { return handle_; }

 private:
  CURL* handle_;
};

class CurlHeaders {
 public:
  CurlHeaders() {
    append("Content-Type: application/json");
    append("Accept: application/json");
  }
  ~CurlHeaders() { curl_slist_free_all(headers_); }
  CurlHeaders(const CurlHeaders&) = delete;
  CurlHeaders& operator=(const CurlHeaders&) = delete;

  [[nodiscard]] curl_slist* get() const noexcept { return headers_; }

 private:
  void append(const char* header) {
    auto* updated = curl_slist_append(headers_, header);
    if (updated == nullptr) {
      curl_slist_free_all(headers_);
      headers_ = nullptr;
      throw HttpError("could not create local HTTP headers");
    }
    headers_ = updated;
  }

  curl_slist* headers_{nullptr};
};

void require_option(const CURLcode status) {
  if (status != CURLE_OK) {
    throw HttpError("could not configure local HTTP request");
  }
}

}  // namespace

HttpResponse CurlHttpClient::post_json(const HttpRequest& request) {
  bool timeout_exceeds_long = false;
  if constexpr (std::numeric_limits<long>::max() <
                std::numeric_limits<std::uint32_t>::max()) {
    timeout_exceeds_long =
        request.timeout_seconds >
        static_cast<std::uint32_t>(std::numeric_limits<long>::max());
  }
  if (request.json_body.size() >
          static_cast<std::size_t>(
              std::numeric_limits<curl_off_t>::max()) ||
      timeout_exceeds_long) {
    throw HttpError("local model HTTP request exceeds supported limits");
  }
  ensure_curl_initialized();
  CurlHandle handle;
  CurlHeaders headers;
  ResponseBuffer response;

  // The adapter never configures credentials, cookies, redirects, proxies, or
  // filesystem output. Its complete capability is one bounded JSON POST.
  require_option(curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str()));
  require_option(curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get()));
  require_option(curl_easy_setopt(handle.get(), CURLOPT_POST, 1L));
  require_option(curl_easy_setopt(
      handle.get(), CURLOPT_POSTFIELDS, request.json_body.data()));
  require_option(curl_easy_setopt(
      handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
      static_cast<curl_off_t>(request.json_body.size())));
  require_option(curl_easy_setopt(
      handle.get(), CURLOPT_TIMEOUT,
      static_cast<long>(request.timeout_seconds)));
  require_option(curl_easy_setopt(
      handle.get(), CURLOPT_CONNECTTIMEOUT,
      static_cast<long>(std::min(request.timeout_seconds, 30U))));
  require_option(curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L));
  // Redirects are explicitly disabled so source excerpts cannot be forwarded
  // from the configured Ollama endpoint to an unexpected host.
  require_option(curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L));
  // A local Ollama request must not be routed through an environment proxy.
  require_option(curl_easy_setopt(handle.get(), CURLOPT_PROXY, ""));
  require_option(curl_easy_setopt(
      handle.get(), CURLOPT_PROTOCOLS_STR, "http,https"));
  require_option(curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION,
                                  &append_response));
  require_option(curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response));

  const auto status = curl_easy_perform(handle.get());
  if (status != CURLE_OK) {
    throw HttpError(response.exceeded_limit
                        ? "local model HTTP response exceeded size limit"
                        : "local model HTTP transport failed");
  }

  long status_code = 0;
  require_option(curl_easy_getinfo(
      handle.get(), CURLINFO_RESPONSE_CODE, &status_code));
  return {status_code, std::move(response.body)};
}

}  // namespace kc::models
