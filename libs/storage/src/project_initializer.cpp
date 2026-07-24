#include "kc/storage/project_initializer.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/json.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/migration_runner.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace kc::storage {
namespace {

/// Read an existing `kc.json`, then strictly parse and validate its contract.
domain::ProjectConfig read_config(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw StorageError("could not read existing project configuration: " + path.string());
  }

  nlohmann::json document;
  try {
    input >> document;
  } catch (const nlohmann::json::exception& error) {
    throw StorageError("invalid project configuration JSON: " + std::string(error.what()));
  }
  const auto parsed = domain::parse_project_config(document);
  if (!parsed) {
    const auto message = parsed.issues.empty() ? "unknown validation error" : parsed.issues.front().message;
    throw StorageError("invalid project configuration: " + message);
  }
  return *parsed.value;
}

/// Atomically create `kc.json` by writing a temporary sibling and renaming it.
///
/// Readers therefore see either the complete old file or the complete new one,
/// never a partly written configuration.
void write_config(const std::filesystem::path& path, const domain::ProjectConfig& config) {
  nlohmann::json document = config;
  const auto temporary = path.parent_path() /
                         (path.filename().string() + ".tmp-" + domain::generate_prefixed_ulid(""));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw StorageError("could not create project configuration: " + path.string());
    }
    output << document.dump(2) << '\n';
    output.flush();
    if (!output) {
      throw StorageError("could not write project configuration: " + path.string());
    }
  }
  std::filesystem::rename(temporary, path);
}

/// Construct the default configuration and validate user-provided vault input.
domain::ProjectConfig new_config(const std::filesystem::path& vault_path) {
  domain::ProjectConfig config;
  config.project_id = domain::generate_project_id();
  config.paths.vault = vault_path.lexically_normal();
  const auto validation = domain::validate(config);
  if (!validation) {
    const auto message = validation.issues.empty() ? "invalid initialization options"
                                                   : validation.issues.front().path + " " +
                                                         validation.issues.front().message;
    throw StorageError(message);
  }
  return config;
}

/// Create every directory promised by the project layout.
///
/// `create_directories` is idempotent, so existing directories are harmless.
void create_project_directories(const std::filesystem::path& root, const domain::ProjectConfig& config) {
  std::filesystem::create_directories(root / config.paths.sources);
  std::filesystem::create_directories(root / config.paths.vault);
  std::filesystem::create_directories(root / config.paths.cache / "pages");
  std::filesystem::create_directories(root / config.paths.cache / "backups");
  std::filesystem::create_directories(root / config.paths.cache / "logs");
  std::filesystem::create_directories((root / config.paths.state).parent_path());
}

}  // namespace

InitResult initialize_project(const InitOptions& options) {
  // Validate required inputs before performing filesystem work.
  if (options.project_root.empty()) {
    throw StorageError("project root must not be empty");
  }
  if (options.migration_directory.empty()) {
    throw StorageError("migration directory must not be empty");
  }

  const auto root = std::filesystem::absolute(options.project_root).lexically_normal();
  std::filesystem::create_directories(root);
  const auto config_path = root / "kc.json";
  const auto created = !std::filesystem::exists(config_path);
  // An existing configuration is authoritative; a repeated `init --vault`
  // validates the project instead of silently changing its vault location.
  auto config = created ? new_config(options.vault_path) : read_config(config_path);

  create_project_directories(root, config);
  if (created) {
    write_config(config_path, config);
  }

  const auto state_path = root / config.paths.state;
  Database database(state_path);
  MigrationRunner migrations(database, options.migration_directory);
  auto migration_result = migrations.apply_all();

  return {std::move(config), config_path, state_path, created,
          std::move(migration_result.applied_versions)};
}

}  // namespace kc::storage
