#pragma once

#include "kc/domain/types.hpp"
#include "kc/storage/database.hpp"

#include <filesystem>
#include <vector>

namespace kc::storage {

struct InitOptions {
  /// Directory that will contain the configuration, database, sources, and vault.
  std::filesystem::path project_root;
  /// Vault location relative to `project_root`.
  std::filesystem::path vault_path{"vault"};
  /// Directory containing versioned SQL migration files.
  std::filesystem::path migration_directory;
};

struct InitResult {
  /// The new configuration, or the validated existing configuration.
  domain::ProjectConfig config;
  std::filesystem::path config_path;
  std::filesystem::path state_path;
  bool created{false};
  std::vector<int> applied_migrations;
};

/// Create or validate a local Knowledge Compiler project.
///
/// The function creates the directory layout and `kc.json` when needed, opens
/// the SQLite state database, and applies pending migrations. Re-running it is
/// intentionally idempotent and does not contact model or OCR services.
[[nodiscard]] InitResult initialize_project(const InitOptions& options);

}  // namespace kc::storage
