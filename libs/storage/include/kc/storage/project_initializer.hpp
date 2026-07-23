#pragma once

#include "kc/domain/types.hpp"
#include "kc/storage/database.hpp"

#include <filesystem>
#include <vector>

namespace kc::storage {

struct InitOptions {
  std::filesystem::path project_root;
  std::filesystem::path vault_path{"vault"};
  std::filesystem::path migration_directory;
};

struct InitResult {
  domain::ProjectConfig config;
  std::filesystem::path config_path;
  std::filesystem::path state_path;
  bool created{false};
  std::vector<int> applied_migrations;
};

[[nodiscard]] InitResult initialize_project(const InitOptions& options);

}  // namespace kc::storage
