#include "kc/compiler/compiler.hpp"
#include "kc/extraction/extractor.hpp"
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

/// Build the machine-readable page extraction result.
nlohmann::json extraction_success_json(
    const kc::extraction::ExtractionResult& result) {
  auto pages = nlohmann::json::array();
  for (const auto& page : result.pages) {
    nlohmann::json image_path = nullptr;
    if (page.image_path) {
      image_path = page.image_path->generic_string();
    }
    pages.push_back(
        {{"page_id", page.page_id.value},
         {"page_number", page.page_number},
         {"image_path", std::move(image_path)},
         {"text", page.text},
         {"text_status", kc::domain::to_string(page.text_status)}});
  }
  return {
      {"ok", true},
      {"command", "extract"},
      {"result",
       {{"source_id", result.source_id.value},
        {"source_version_id", result.source_version_id.value},
        {"run_id", result.run_id.value.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json(result.run_id.value)},
        {"page_count", result.pages.size()},
        {"failed_pages", result.failed_pages},
        {"reused", result.reused},
        {"pages", std::move(pages)}}}};
}

/// Report durable failed page rows while retaining the CLI error contract.
nlohmann::json extraction_failure_json(
    const kc::extraction::ExtractionResult& result) {
  return {
      {"ok", false},
      {"command", "extract"},
      {"error",
       {{"code", "ocr_failed"},
        {"message", "OCR failed or produced unusable text for one or more pages"},
        {"source_id", result.source_id.value},
        {"source_version_id", result.source_version_id.value},
        {"failed_pages", result.failed_pages},
        {"page_count", result.pages.size()}}}};
}

/// Build the machine-readable result for a committed pending proposal.
nlohmann::json compile_success_json(
    const kc::compiler::CompileResult& result) {
  nlohmann::json article_id = nullptr;
  if (result.article_id) {
    article_id = result.article_id->value;
  }
  return {
      {"ok", true},
      {"command", "compile"},
      {"result",
       {{"proposal_id", result.proposal_id.value},
        {"model_run_id", result.model_run_id.value},
        {"operation", kc::domain::to_string(result.operation)},
        {"article_id", std::move(article_id)},
        {"selected_page_count", result.selected_page_count},
        {"status", "pending"}}}};
}

/// Map compiler failures to the stable process exits in CLI_CONTRACT.md.
int compiler_exit_code(const kc::compiler::CompilerErrorKind kind) {
  switch (kind) {
    case kc::compiler::CompilerErrorKind::invalid_project:
    case kc::compiler::CompilerErrorKind::unsupported_concept:
      return 2;
    case kc::compiler::CompilerErrorKind::source_not_found:
    case kc::compiler::CompilerErrorKind::no_evidence:
      return 3;
    case kc::compiler::CompilerErrorKind::model_failed:
      return 4;
    case kc::compiler::CompilerErrorKind::validation_failed:
      return 5;
    case kc::compiler::CompilerErrorKind::state_error:
      return 6;
  }
  return 6;
}

/// Map compiler failures to the machine-readable CLI error vocabulary.
std::string_view compiler_error_code(
    const kc::compiler::CompilerErrorKind kind) {
  switch (kind) {
    case kc::compiler::CompilerErrorKind::invalid_project:
      return "invalid_project";
    case kc::compiler::CompilerErrorKind::unsupported_concept:
      return "unsupported_concept";
    case kc::compiler::CompilerErrorKind::source_not_found:
      return "source_not_found";
    case kc::compiler::CompilerErrorKind::no_evidence:
      return "no_evidence";
    case kc::compiler::CompilerErrorKind::model_failed:
      return "model_failed";
    case kc::compiler::CompilerErrorKind::validation_failed:
      return "validation_failed";
    case kc::compiler::CompilerErrorKind::state_error:
      return "state_error";
  }
  return "state_error";
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

  auto* extract_command =
      app.add_subcommand("extract", "Extract page text and retain PDF page images");
  extract_command->fallthrough();
  std::string extract_source_id;
  bool force_extraction = false;
  extract_command->add_option("SOURCE_ID", extract_source_id, "Imported source ID")
      ->required();
  extract_command->add_flag(
      "--force", force_extraction, "Replace existing extraction state");

  auto* compile_command = app.add_subcommand(
      "compile", "Select evidence and create a validated pending proposal");
  compile_command->fallthrough();
  std::string concept_title;
  std::vector<std::string> compile_source_ids;
  compile_command->add_option("--concept", concept_title, "Concept title")
      ->required();
  compile_command
      ->add_option("--source", compile_source_ids,
                   "Restrict evidence to one or more imported source IDs")
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

  if (*extract_command) {
    try {
      if (project_option->count() == 0U) {
        const auto discovered =
            find_project_root(std::filesystem::current_path());
        if (!discovered) {
          throw kc::extraction::ExtractionError(
              kc::extraction::ExtractionErrorKind::invalid_project,
              "no kc.json found in the current directory or its parents; run 'kc init' first");
        }
        project = *discovered;
      }

      kc::extraction::Extractor extractor(project);
      const auto result = extractor.extract(
          kc::domain::SourceId{extract_source_id}, force_extraction);
      if (result.failed_pages != 0U) {
        if (json_output) {
          std::cout << extraction_failure_json(result).dump() << '\n';
        } else {
          std::cerr << "kc extract: " << result.failed_pages
                    << " page(s) failed OCR; failed page state was retained\n";
        }
        return 4;
      }

      if (json_output) {
        std::cout << extraction_success_json(result).dump() << '\n';
      } else if (!quiet) {
        std::cout << (result.reused ? "Found " : "Extracted ")
                  << result.pages.size() << " page(s) for "
                  << result.source_id.value << '\n';
      }
      return 0;
    } catch (const kc::extraction::ExtractionError& error) {
      const auto kind = error.kind();
      const auto exit_code =
          kind == kc::extraction::ExtractionErrorKind::invalid_project
              ? 2
              : kind == kc::extraction::ExtractionErrorKind::source_not_found
                    ? 3
                    : kind == kc::extraction::ExtractionErrorKind::adapter_error
                          ? 4
                          : 6;
      const auto code =
          kind == kc::extraction::ExtractionErrorKind::invalid_project
              ? "invalid_project"
              : kind == kc::extraction::ExtractionErrorKind::source_not_found
                    ? "source_not_found"
                    : kind == kc::extraction::ExtractionErrorKind::adapter_error
                          ? "adapter_failed"
                          : "state_error";
      if (json_output) {
        std::cout << error_json("extract", code, error.what()).dump() << '\n';
      } else {
        std::cerr << "kc extract: " << error.what() << '\n';
      }
      return exit_code;
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("extract", "internal_error", error.what()).dump()
                  << '\n';
      } else {
        std::cerr << "kc extract: unexpected error: " << error.what() << '\n';
      }
      return 6;
    }
  }

  if (*compile_command) {
    try {
      if (project_option->count() == 0U) {
        const auto discovered =
            find_project_root(std::filesystem::current_path());
        if (!discovered) {
          throw kc::compiler::CompilerError(
              kc::compiler::CompilerErrorKind::invalid_project,
              "no kc.json found in the current directory or its parents; "
              "run 'kc init' first");
        }
        project = *discovered;
      }

      kc::compiler::CompileOptions options{.concept_title = concept_title};
      options.source_ids.reserve(compile_source_ids.size());
      for (auto& source_id : compile_source_ids) {
        options.source_ids.push_back({std::move(source_id)});
      }
      kc::compiler::Compiler compiler(project);
      const auto result = compiler.compile(options);
      if (json_output) {
        std::cout << compile_success_json(result).dump() << '\n';
      } else if (!quiet) {
        std::cout << "Created pending proposal " << result.proposal_id.value
                  << " from " << result.selected_page_count
                  << " relevant page(s)\n";
      }
      return 0;
    } catch (const kc::compiler::CompilerError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout
            << error_json("compile", compiler_error_code(kind), error.what())
                   .dump()
            << '\n';
      } else {
        std::cerr << "kc compile: " << error.what() << '\n';
      }
      return compiler_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("compile", "internal_error", error.what()).dump()
                  << '\n';
      } else {
        std::cerr << "kc compile: unexpected error: " << error.what() << '\n';
      }
      return 6;
    }
  }

  return 2;
}
