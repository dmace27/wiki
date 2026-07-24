# The storage library owns SQLite access, migrations, and project initialization.
add_library(kc_storage STATIC
  src/database.cpp
  src/migration_runner.cpp
  src/project_initializer.cpp
)

add_library(kc::storage ALIAS kc_storage)

target_include_directories(
  kc_storage
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(
  kc_storage
  PUBLIC kc::domain
  PRIVATE unofficial::sqlite3::sqlite3 kc_project_options
)
