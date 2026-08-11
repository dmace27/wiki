# Build the user-facing `kc` command from its single entry point.
add_executable(kc main.cpp)

target_link_libraries(
  kc
  PRIVATE
    CLI11::CLI11
    kc::compiler
    kc::extraction
    kc::import
    kc::storage
    kc::vault
    kc_project_options
)

# Embed development and installation migration locations in the executable.
target_compile_definitions(
  kc
  PRIVATE
    KC_SOURCE_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/migrations"
    KC_INSTALL_MIGRATIONS_SUBDIR="${CMAKE_INSTALL_DATADIR}/kc/migrations"
)

install(TARGETS kc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
