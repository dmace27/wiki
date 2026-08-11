# The search library provides read-only, concept-first access to the FTS5
# index populated transactionally by the vault writer.
add_library(kc_search STATIC
  src/search_index.cpp
)

add_library(kc::search ALIAS kc_search)

target_include_directories(
  kc_search
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_search
  PUBLIC kc::domain
  PRIVATE
    kc::storage
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    kc_project_options
)
