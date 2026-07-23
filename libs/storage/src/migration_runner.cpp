#include "kc/storage/migration_runner.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kc::storage {
namespace {

struct Migration {
  int version;
  std::filesystem::path path;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw StorageError("could not read migration: " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::vector<Migration> discover(const std::filesystem::path& directory) {
  if (!std::filesystem::is_directory(directory)) {
    throw StorageError("migration directory does not exist: " + directory.string());
  }

  const std::regex filename_pattern(R"(^([0-9]+)_[A-Za-z0-9_-]+\.sql$)");
  std::vector<Migration> migrations;
  std::set<int> versions;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::smatch match;
    const auto filename = entry.path().filename().string();
    if (!std::regex_match(filename, match, filename_pattern)) {
      continue;
    }
    const auto version = std::stoi(match[1].str());
    if (!versions.insert(version).second) {
      throw StorageError("duplicate migration version: " + std::to_string(version));
    }
    migrations.push_back({version, entry.path()});
  }
  std::ranges::sort(migrations, {}, &Migration::version);
  return migrations;
}

bool has_migration_table(Database& database) {
  return database.scalar_integer(
             "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'schema_migrations';") == 1;
}

std::set<int> applied_versions(Database& database) {
  std::set<int> versions;
  if (!has_migration_table(database)) {
    return versions;
  }

  sqlite3_stmt* statement = nullptr;
  constexpr auto sql = "SELECT version FROM schema_migrations ORDER BY version";
  if (sqlite3_prepare_v2(database.native_handle(), sql, -1, &statement, nullptr) != SQLITE_OK) {
    throw StorageError("failed to read applied migrations");
  }
  while (sqlite3_step(statement) == SQLITE_ROW) {
    versions.insert(sqlite3_column_int(statement, 0));
  }
  sqlite3_finalize(statement);
  return versions;
}

void record_migration(Database& database, const int version) {
  sqlite3_stmt* statement = nullptr;
  constexpr auto sql = "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)";
  if (sqlite3_prepare_v2(database.native_handle(), sql, -1, &statement, nullptr) != SQLITE_OK) {
    throw StorageError("failed to prepare migration record");
  }
  const auto timestamp = utc_now();
  sqlite3_bind_int(statement, 1, version);
  sqlite3_bind_text(statement, 2, timestamp.c_str(), -1, SQLITE_TRANSIENT);
  const auto status = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (status != SQLITE_DONE) {
    throw StorageError("failed to record applied migration " + std::to_string(version));
  }
}

}  // namespace

MigrationRunner::MigrationRunner(Database& database, std::filesystem::path migration_directory)
    : database_(database), migration_directory_(std::move(migration_directory)) {}

MigrationResult MigrationRunner::apply_all() {
  database_.execute("PRAGMA foreign_keys = ON;");
  database_.execute("PRAGMA journal_mode = WAL;");

  const auto migrations = discover(migration_directory_);
  auto applied = applied_versions(database_);
  MigrationResult result;

  for (const auto& migration : migrations) {
    if (applied.contains(migration.version)) {
      continue;
    }

    database_.execute("BEGIN IMMEDIATE;");
    try {
      database_.execute(read_file(migration.path));
      record_migration(database_, migration.version);
      database_.execute("COMMIT;");
      result.applied_versions.push_back(migration.version);
      applied.insert(migration.version);
    } catch (...) {
      try {
        database_.execute("ROLLBACK;");
      } catch (...) {
      }
      throw;
    }
  }
  return result;
}

}  // namespace kc::storage

