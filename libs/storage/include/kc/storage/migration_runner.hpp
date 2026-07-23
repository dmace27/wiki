#pragma once

#include "kc/storage/database.hpp"

#include <filesystem>
#include <vector>

namespace kc::storage {

struct MigrationResult {
  std::vector<int> applied_versions;
};

class MigrationRunner {
 public:
  MigrationRunner(Database& database, std::filesystem::path migration_directory);

  [[nodiscard]] MigrationResult apply_all();

 private:
  Database& database_;
  std::filesystem::path migration_directory_;
};

}  // namespace kc::storage

