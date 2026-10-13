# Compile authoritative schemas into the domain library. Register the inputs so
# editing a public schema automatically reconfigures every affected build.
foreach(schema IN ITEMS project-config extracted-page citation article-proposal)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/schemas/${schema}.schema.json")
endforeach()
file(READ "${PROJECT_SOURCE_DIR}/schemas/project-config.schema.json" KC_CONFIG_SCHEMA)
file(READ "${PROJECT_SOURCE_DIR}/schemas/extracted-page.schema.json" KC_PAGE_SCHEMA)
file(READ "${PROJECT_SOURCE_DIR}/schemas/citation.schema.json" KC_CITATION_SCHEMA)
file(READ "${PROJECT_SOURCE_DIR}/schemas/article-proposal.schema.json" KC_PROPOSAL_SCHEMA)
configure_file(src/schema_documents.hpp.in generated/schema_documents.hpp @ONLY)

# The domain library owns IDs, value types, validation, and JSON conversion.
add_library(kc_domain STATIC
  src/concept.cpp
  src/id.cpp
  src/json.cpp
  src/markdown.cpp
  src/sha256.cpp
  src/schema.cpp
  src/text.cpp
  src/types.cpp
)

add_library(kc::domain ALIAS kc_domain)

# Expose public headers to consumers while keeping build/install paths portable.
target_include_directories(
  kc_domain
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_domain
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE utf8proc::utf8proc nlohmann_json_schema_validator::validator kc_project_options
)
