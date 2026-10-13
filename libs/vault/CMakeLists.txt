# The vault library is the only layer allowed to turn an approved structured
# proposal into a user-owned Markdown file.
add_library(kc_vault STATIC
  src/vault_writer.cpp
  src/rendering.cpp
)

add_library(kc::vault ALIAS kc_vault)

target_include_directories(
  kc_vault
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_vault
  PUBLIC kc::domain
  PRIVATE
    kc::updates
    kc::import
    kc::storage
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    unofficial::sqlite3::sqlite3
    kc_project_options
)
