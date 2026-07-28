# The extraction library owns native text extraction, PDF page rendering,
# local OCR adapters, and page-level persistence.
add_library(kc_extraction STATIC
  src/adapters.cpp
  src/extractor.cpp
)

add_library(kc::extraction ALIAS kc_extraction)

target_include_directories(
  kc_extraction
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_extraction
  PUBLIC kc::domain
  PRIVATE
    kc::import
    kc::storage
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    kc_project_options
)
