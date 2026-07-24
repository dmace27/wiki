#pragma once

#include "kc/storage/database.hpp"

#include <filesystem>
#include <vector>

namespace kc::storage {

struct MigrationResult {
  /// Versions installed by this run; already-installed versions are omitted.
  std::vector<int> applied_versions;
};

/// Finds numbered `.sql` files and applies pending database changes in order.
class MigrationRunner {
 public:
  /// Keep a reference to the target database and remember where migrations live.
  MigrationRunner(Database& database, std::filesystem::path migration_directory);

  /// Apply every pending migration transactionally.
  ///
  /// If a migration fails, that migration is rolled back and the exception is
  /// propagated. Calling this method again is safe because applied versions are
  /// recorded in `schema_migrations`.
  [[nodiscard]] MigrationResult apply_all();

 private:
  Database& database_;
  std::filesystem::path migration_directory_;
};

}  // namespace kc::storage
