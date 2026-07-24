# The domain library owns IDs, value types, validation, and JSON conversion.
add_library(kc_domain STATIC
  src/id.cpp
  src/json.cpp
  src/types.cpp
)

add_library(kc::domain ALIAS kc_domain)

# Expose public headers to consumers while keeping build/install paths portable.
target_include_directories(
  kc_domain
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_domain
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE kc_project_options
)
