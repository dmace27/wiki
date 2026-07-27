# The import library owns hashing, immutable file retention, and source
# registration in project state.
add_library(kc_import STATIC
  src/sha256.cpp
  src/source_importer.cpp
)

add_library(kc::import ALIAS kc_import)

target_include_directories(
  kc_import
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_import
  PUBLIC kc::domain
  PRIVATE
    kc::storage
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    kc_project_options
)
