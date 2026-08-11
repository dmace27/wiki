# The compiler library coordinates deterministic evidence selection, validated
# model generation, and transactional proposal persistence.
add_library(kc_compiler STATIC
  src/compiler.cpp
)

add_library(kc::compiler ALIAS kc_compiler)

target_include_directories(
  kc_compiler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_compiler
  PUBLIC
    kc::domain
    kc::models
  PRIVATE
    kc::import
    kc::storage
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    kc_project_options
)
