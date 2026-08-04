# The models library owns the filesystem-isolated LanguageModel boundary,
# Ollama HTTP integration, structured Markov Chains prompt, response
# validation, and auditable model-run persistence.
add_library(kc_models STATIC
  src/curl_http_client.cpp
  src/markov_chains_prompt.cpp
  src/model_runner.cpp
  src/ollama_language_model.cpp
  src/proposal_validator.cpp
)

add_library(kc::models ALIAS kc_models)

target_include_directories(
  kc_models
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_models
  PUBLIC
    kc::domain
    nlohmann_json::nlohmann_json
  PRIVATE
    CURL::libcurl
    kc::import
    kc::storage
    unofficial::sqlite3::sqlite3
    kc_project_options
)
