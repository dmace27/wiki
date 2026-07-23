#include "kc/storage/project_initializer.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path migration_directory(const char* executable) {
  const auto executable_path = std::filesystem::absolute(executable).lexically_normal();
  const auto installed = executable_path.parent_path().parent_path() / KC_INSTALL_MIGRATIONS_SUBDIR;
  if (std::filesystem::is_directory(installed)) {
    return installed;
  }
  return KC_SOURCE_MIGRATIONS_DIR;
}

nlohmann::json success_json(const kc::storage::InitResult& result) {
  return {
      {"ok", true},
      {"command", "init"},
      {"result",
       {{"project_id", result.config.project_id.value},
        {"created", result.created},
        {"config_path", result.config_path.string()},
        {"state_path", result.state_path.string()},
        {"applied_migrations", result.applied_migrations}}}};
}

nlohmann::json error_json(const std::string_view command, const std::string_view code,
                          const std::string_view message) {
  return {{"ok", false},
          {"command", command},
          {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

int main(const int argc, char** argv) {
  CLI::App app{"Compile personal learning sources into a source-grounded knowledge base", "kc"};
  app.set_version_flag("--version", "kc 0.1.0");
  app.require_subcommand(1);

  std::filesystem::path project = std::filesystem::current_path();
  bool json_output = false;
  bool quiet = false;
  app.add_option("--project", project, "Project root");
  app.add_flag("--json", json_output, "Emit one JSON document to standard output");
  app.add_flag("--quiet", quiet, "Suppress progress output");

  auto* init = app.add_subcommand("init", "Create a project and initialize its state database");
  init->fallthrough();
  std::filesystem::path vault = "vault";
  init->add_option("--vault", vault, "Project-relative Obsidian vault path");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    app.exit(error);
    return 2;
  }

  if (*init) {
    try {
      const auto result = kc::storage::initialize_project(
          {.project_root = project,
           .vault_path = vault,
           .migration_directory = migration_directory(argv[0])});
      if (json_output) {
        std::cout << success_json(result).dump() << '\n';
      } else if (!quiet) {
        std::cout << (result.created ? "Initialized" : "Validated") << " knowledge compiler project at "
                  << std::filesystem::absolute(project).lexically_normal().string() << '\n';
        if (!result.applied_migrations.empty()) {
          std::cout << "Applied " << result.applied_migrations.size() << " migration(s).\n";
        }
      }
      return 0;
    } catch (const kc::storage::StorageError& error) {
      if (json_output) {
        std::cout << error_json("init", "invalid_project", error.what()).dump() << '\n';
      } else {
        std::cerr << "kc init: " << error.what() << '\n';
      }
      return 2;
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("init", "internal_error", error.what()).dump() << '\n';
      } else {
        std::cerr << "kc init: unexpected error: " << error.what() << '\n';
      }
      return 6;
    }
  }

  return 2;
}
