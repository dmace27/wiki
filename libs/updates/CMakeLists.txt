# Shared accumulation and base-revision checks; never writes article files.
add_library(kc_updates STATIC src/article_updates.cpp)
add_library(kc::updates ALIAS kc_updates)
target_include_directories(kc_updates PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(kc_updates PUBLIC kc::domain PRIVATE
  kc::import kc::storage unofficial::sqlite3::sqlite3 kc_project_options)
