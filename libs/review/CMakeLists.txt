# The review library owns human inspection and review-state transitions. It
# may correct extracted page text, but it deliberately has no vault dependency.
add_library(kc_review STATIC
  src/review_service.cpp
)

add_library(kc::review ALIAS kc_review)

target_include_directories(
  kc_review
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_review
  PUBLIC kc::domain
  PRIVATE
    kc::import
    kc::storage
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    kc_project_options
)
