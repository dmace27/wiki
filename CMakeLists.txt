cmake_minimum_required(VERSION 3.25)

# Define the root project and require C++ as its implementation language.
project(
  knowledge_compiler
  VERSION 0.1.0
  DESCRIPTION "Local-first compiler for source-grounded Obsidian knowledge bases"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)
include(ProjectOptions)

option(KC_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# An interface library carries common compiler settings without producing a
# library file of its own.
add_library(kc_project_options INTERFACE)
target_compile_features(kc_project_options INTERFACE cxx_std_23)
kc_enable_warnings(kc_project_options ${KC_WARNINGS_AS_ERRORS})
if(KC_ENABLE_SANITIZERS)
  kc_enable_sanitizers(kc_project_options)
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Build each layer after its third-party dependencies are available.
add_subdirectory(libs/domain)
add_subdirectory(libs/storage)
add_subdirectory(libs/import)
add_subdirectory(libs/extraction)
add_subdirectory(apps/kc)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(
  DIRECTORY migrations/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/kc/migrations"
  FILES_MATCHING PATTERN "*.sql"
)
