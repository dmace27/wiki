#include "kc/import/source_importer.hpp"
#include "kc/storage/project_initializer.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// Locate SQL migrations beside an installed executable, with the source tree
/// as a development-build fallback.
std::filesystem::path migration_directory(const char* executable) {
  const auto executable_path = std::filesystem::absolute(executable).lexically_normal();
  const auto installed = executable_path.parent_path().parent_path() / KC_INSTALL_MIGRATIONS_SUBDIR;
  if (std::filesystem::is_directory(installed)) {
    return installed;
  }
  return KC_SOURCE_MIGRATIONS_DIR;
}

/// Build the stable machine-readable response for a successful `kc init`.
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

/// Build the common machine-readable error shape required by the CLI contract.
nlohmann::json error_json(const std::string_view command, const std::string_view code,
                          const std::string_view message) {
  return {{"ok", false},
          {"command", command},
          {"error", {{"code", code}, {"message", message}}}};
}

/// Find the closest initialized project, starting at the current directory.
std::optional<std::filesystem::path> find_project_root(
    std::filesystem::path directory) {
  directory = std::filesystem::absolute(std::move(directory)).lexically_normal();
  while (true) {
    if (std::filesystem::is_regular_file(directory / "kc.json")) {
      return directory;
    }
    const auto parent = directory.parent_path();
    if (parent == directory) {
      return std::nullopt;
    }
    directory = parent;
  }
}

/// Build the stable machine-readable result for one or more source imports.
nlohmann::json import_success_json(
    const std::vector<kc::source_import::ImportResult>& results) {
  auto imported = nlohmann::json::array();
  for (const auto& result : results) {
    imported.push_back(
        {{"source_id", result.source.source_id.value},
         {"source_version_id", result.version.source_version_id.value},
         {"display_name", result.source.display_name},
         {"source_kind", kc::domain::to_string(result.source.source_kind)},
         {"sha256", result.version.sha256},
         {"stored_path", result.version.stored_path.generic_string()},
         {"media_type", result.version.media_type},
         {"byte_size", result.version.byte_size},
         {"source_created", result.source_created},
         {"version_created", result.version_created},
         {"deduplicated", !result.version_created}});
  }
  return {{"ok", true},
          {"command", "import"},
          {"result", {{"sources", std::move(imported)}}}};
}

}  // namespace

int main(const int argc, char** argv) {
  // Define the command-line interface before parsing so CLI11 can generate help
  // and validate required subcommands consistently.
  CLI::App app{"Compile personal learning sources into a source-grounded knowledge base", "kc"};
  app.set_version_flag("--version", "kc 0.1.0");
  app.require_subcommand(1);

  std::filesystem::path project = std::filesystem::current_path();
  bool json_output = false;
  bool quiet = false;
  auto* project_option = app.add_option("--project", project, "Project root");
  app.add_flag("--json", json_output, "Emit one JSON document to standard output");
  app.add_flag("--quiet", quiet, "Suppress progress output");

  auto* init = app.add_subcommand("init", "Create a project and initialize its state database");
  init->fallthrough();
  std::filesystem::path vault = "vault";
  init->add_option("--vault", vault, "Project-relative Obsidian vault path");

  auto* import_command =
      app.add_subcommand("import", "Register sources and retain immutable project-local copies");
  import_command->fallthrough();
  std::vector<std::filesystem::path> source_files;
  import_command->add_option("FILE", source_files, "Markdown, text, or PDF source file")
      ->required()
      ->expected(-1);

  // Parsing the CLI inputs
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    app.exit(error);
    return 2;
  }

  if (*init) {
    try {
      // Initialization is deliberately offline: it only creates local files
      // and applies SQLite migrations.
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
      return 0; // completed successfully
    }

    catch (const kc::storage::StorageError& error) {
      if (json_output) {
        std::cout << error_json("init", "invalid_project", error.what()).dump() << '\n';
      } else {
        std::cerr << "kc init: " << error.what() << '\n';
      }
      return 2; // invalid command arguments or invalid project config
    }

    catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("init", "internal_error", error.what()).dump() << '\n';
      } else {
        std::cerr << "kc init: unexpected error: " << error.what() << '\n';
      }
      return 6; // unexpected internal error
    }
  }

  if (*import_command) {
    try {
      if (project_option->count() == 0U) {
        const auto discovered = find_project_root(std::filesystem::current_path());
        if (!discovered) {
          throw kc::source_import::ImportError(
              kc::source_import::ImportErrorKind::invalid_project,
              "no kc.json found in the current directory or its parents; run 'kc init' first");
        }
        project = *discovered;
      }

      kc::source_import::SourceImporter importer(project);
      const auto results = importer.import_files(source_files);
      if (json_output) {
        std::cout << import_success_json(results).dump() << '\n';
      } else if (!quiet) {
        for (const auto& result : results) {
          std::cout << (result.version_created ? "Imported " : "Already imported ")
                    << result.source.display_name << " as "
                    << result.source.source_id.value << " ("
                    << result.version.source_version_id.value << ")\n";
        }
      }
      return 0;
    } catch (const kc::source_import::ImportError& error) {
      const auto unavailable =
          error.kind() == kc::source_import::ImportErrorKind::unavailable_input;
      const auto state_error =
          error.kind() == kc::source_import::ImportErrorKind::state_error;
      const auto code = unavailable ? "source_unavailable" : "invalid_import";
      if (json_output) {
        std::cout << error_json(
                         "import", state_error ? "state_error" : code, error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc import: " << error.what() << '\n';
      }
      return state_error ? 6 : (unavailable ? 3 : 2);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("import", "internal_error", error.what()).dump()
                  << '\n';
      } else {
        std::cerr << "kc import: unexpected error: " << error.what() << '\n';
      }
      return 6;
    }
  }

  return 2;
}
