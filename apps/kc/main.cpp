#include "kc/compiler/compiler.hpp"
#include "kc/domain/json.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/review/review_service.hpp"
#include "kc/storage/project_initializer.hpp"
#include "kc/vault/vault_writer.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

/// Serialize extraction evidence without separating image, text, and status.
nlohmann::json extraction_review_success_json(
    const kc::review::ExtractionReview& review,
    const std::optional<kc::review::PageCorrectionResult>& correction,
    const std::optional<std::uint32_t> selected_page) {
  auto pages = nlohmann::json::array();
  for (const auto& page : review.pages) {
    if (selected_page && page.page_number != *selected_page) {
      continue;
    }
    nlohmann::json image_path = nullptr;
    if (page.image_path) {
      image_path = page.image_path->generic_string();
    }
    pages.push_back({{"page_id", page.page_id.value},
                     {"page_number", page.page_number},
                     {"image_path", std::move(image_path)},
                     {"text", page.text},
                     {"text_status", kc::domain::to_string(page.text_status)}});
  }

  nlohmann::json corrected = nullptr;
  if (correction) {
    corrected = {{"page_id", correction->page_id.value},
                 {"page_number", correction->page_number},
                 {"text_sha256", correction->text_sha256}};
  }
  return {{"ok", true},
          {"command", "review extraction"},
          {"result",
           {{"source_id", review.source_id.value},
            {"source_version_id", review.source_version_id.value},
            {"display_name", review.display_name},
            {"source_kind", kc::domain::to_string(review.source_kind)},
            {"corrected", std::move(corrected)},
            {"pages", std::move(pages)}}}};
}

/// Serialize compact proposal metadata shared by list and show output.
nlohmann::json proposal_summary_json(
    const kc::review::ProposalSummary& summary) {
  nlohmann::json article_id = nullptr;
  nlohmann::json reviewed_at = nullptr;
  nlohmann::json review_reason = nullptr;
  if (summary.article_id) {
    article_id = summary.article_id->value;
  }
  if (summary.reviewed_at) {
    reviewed_at = *summary.reviewed_at;
  }
  if (summary.review_reason) {
    review_reason = *summary.review_reason;
  }
  return {{"proposal_id", summary.proposal_id.value},
          {"article_id", std::move(article_id)},
          {"operation", kc::domain::to_string(summary.operation)},
          {"status", kc::domain::to_string(summary.status)},
          {"title", summary.title},
          {"created_at", summary.created_at},
          {"reviewed_at", std::move(reviewed_at)},
          {"review_reason", std::move(review_reason)}};
}

/// Build the machine-readable proposal list used by scripts and future UI.
nlohmann::json proposal_list_success_json(
    const std::vector<kc::review::ProposalSummary>& proposals) {
  auto items = nlohmann::json::array();
  for (const auto& proposal : proposals) {
    items.push_back(proposal_summary_json(proposal));
  }
  return {{"ok", true},
          {"command", "proposal list"},
          {"result", {{"proposals", std::move(items)}}}};
}

/// Include complete extracted source evidence beside normalized citations.
nlohmann::json proposal_review_success_json(
    const kc::review::ProposalReview& review) {
  auto evidence = nlohmann::json::array();
  for (const auto& item : review.citation_evidence) {
    nlohmann::json image_path = nullptr;
    if (item.image_path) {
      image_path = item.image_path->generic_string();
    }
    evidence.push_back(
        {{"section_key", item.section_key},
         {"block_index", item.block_index},
         {"citation",
          {{"page_id", item.citation.page_id.value},
           {"start_char", item.citation.start_char},
           {"end_char", item.citation.end_char},
           {"quote", item.citation.quote}}},
         {"source_id", item.source_id.value},
         {"source_name", item.source_name},
         {"source_kind", kc::domain::to_string(item.source_kind)},
         {"source_version_id", item.source_version_id.value},
         {"page_number", item.page_number},
         {"image_path", std::move(image_path)},
         {"extracted_text", item.extracted_text},
         {"text_status", kc::domain::to_string(item.text_status)}});
  }
  const nlohmann::json proposal = review.proposal;
  return {{"ok", true},
          {"command", "proposal show"},
          {"result",
           {{"summary", proposal_summary_json(review.summary)},
            {"proposal", proposal},
            {"citation_evidence", std::move(evidence)}}}};
}

/// Build the machine-readable result for a rejection review decision.
nlohmann::json rejection_success_json(
    const kc::domain::ProposalId& proposal_id,
    const std::optional<std::string>& reason) {
  nlohmann::json serialized_reason = nullptr;
  if (reason) {
    serialized_reason = *reason;
  }
  return {{"ok", true},
          {"command", "proposal reject"},
          {"result",
           {{"proposal_id", proposal_id.value},
            {"status", "rejected"},
            {"reason", std::move(serialized_reason)}}}};
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

/// Build the machine-readable result for the review-only approval transition.
nlohmann::json approval_success_json(
    const kc::domain::ProposalId& proposal_id) {
  return {{"ok", true},
          {"command", "proposal approve"},
          {"result",
           {{"proposal_id", proposal_id.value}, {"status", "approved"}}}};
}

/// Build the machine-readable result for one durable vault application.
nlohmann::json apply_success_json(const kc::vault::ApplyResult& result) {
  nlohmann::json backup_path = nullptr;
  if (result.backup_path) {
    backup_path = result.backup_path->generic_string();
  }
  return {
      {"ok", true},
      {"command", "apply"},
      {"result",
       {{"apply_run_id", result.apply_run_id.value},
        {"proposal_id", result.proposal_id.value},
        {"article_id", result.article_id.value},
        {"vault_path", result.vault_path.generic_string()},
        {"backup_path", std::move(backup_path)},
        {"content_sha256", result.content_sha256},
        {"status", "applied"}}}};
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

/// Map writer failures to the stable process exits in CLI_CONTRACT.md.
int vault_exit_code(const kc::vault::VaultErrorKind kind) {
  switch (kind) {
    case kc::vault::VaultErrorKind::invalid_project:
      return 2;
    case kc::vault::VaultErrorKind::proposal_not_found:
    case kc::vault::VaultErrorKind::invalid_proposal_state:
    case kc::vault::VaultErrorKind::unsafe_write:
      return 3;
    case kc::vault::VaultErrorKind::validation_failed:
      return 5;
    case kc::vault::VaultErrorKind::state_error:
      return 6;
  }
  return 6;
}

/// Map writer failures to a small, script-friendly error vocabulary.
std::string_view vault_error_code(const kc::vault::VaultErrorKind kind) {
  switch (kind) {
    case kc::vault::VaultErrorKind::invalid_project:
      return "invalid_project";
    case kc::vault::VaultErrorKind::proposal_not_found:
      return "proposal_not_found";
    case kc::vault::VaultErrorKind::invalid_proposal_state:
      return "invalid_proposal_state";
    case kc::vault::VaultErrorKind::validation_failed:
      return "validation_failed";
    case kc::vault::VaultErrorKind::unsafe_write:
      return "unsafe_write";
    case kc::vault::VaultErrorKind::state_error:
      return "state_error";
  }
  return "state_error";
}

/// Map review failures to the stable process exits in CLI_CONTRACT.md.
int review_exit_code(const kc::review::ReviewErrorKind kind) {
  switch (kind) {
    case kc::review::ReviewErrorKind::invalid_project:
    case kc::review::ReviewErrorKind::invalid_input:
      return 2;
    case kc::review::ReviewErrorKind::source_not_found:
    case kc::review::ReviewErrorKind::page_not_found:
    case kc::review::ReviewErrorKind::proposal_not_found:
    case kc::review::ReviewErrorKind::invalid_state:
      return 3;
    case kc::review::ReviewErrorKind::state_error:
      return 6;
  }
  return 6;
}

/// Map review failures to a small, script-friendly error vocabulary.
std::string_view review_error_code(const kc::review::ReviewErrorKind kind) {
  switch (kind) {
    case kc::review::ReviewErrorKind::invalid_project:
      return "invalid_project";
    case kc::review::ReviewErrorKind::source_not_found:
      return "source_not_found";
    case kc::review::ReviewErrorKind::page_not_found:
      return "page_not_found";
    case kc::review::ReviewErrorKind::proposal_not_found:
      return "proposal_not_found";
    case kc::review::ReviewErrorKind::invalid_state:
      return "invalid_state";
    case kc::review::ReviewErrorKind::invalid_input:
      return "invalid_input";
    case kc::review::ReviewErrorKind::state_error:
      return "state_error";
  }
  return "state_error";
}

/// Parse the optional proposal-list status without accepting near matches.
kc::domain::ProposalStatus parse_proposal_status(const std::string_view value) {
  if (value == "pending") {
    return kc::domain::ProposalStatus::pending;
  }
  if (value == "approved") {
    return kc::domain::ProposalStatus::approved;
  }
  if (value == "rejected") {
    return kc::domain::ProposalStatus::rejected;
  }
  if (value == "applied") {
    return kc::domain::ProposalStatus::applied;
  }
  if (value == "superseded") {
    return kc::domain::ProposalStatus::superseded;
  }
  throw kc::review::ReviewError(
      kc::review::ReviewErrorKind::invalid_input,
      "proposal status must be pending, approved, rejected, applied, or superseded");
}

/// Read a reviewer correction exactly, including line breaks.
std::string read_correction_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw kc::review::ReviewError(
        kc::review::ReviewErrorKind::invalid_input,
        "could not read correction text file: " + path.string());
  }
  std::string text{std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw kc::review::ReviewError(
        kc::review::ReviewErrorKind::invalid_input,
        "could not read complete correction text file: " + path.string());
  }
  return text;
}

/// Discover a project while preserving review-specific error classification.
void discover_review_project_if_unspecified(
    std::filesystem::path& project, const CLI::Option& project_option,
    const std::string_view command) {
  if (project_option.count() != 0U) {
    return;
  }
  const auto discovered = find_project_root(std::filesystem::current_path());
  if (!discovered) {
    throw kc::review::ReviewError(
        kc::review::ReviewErrorKind::invalid_project,
        "no kc.json found in the current directory or its parents; run 'kc "
        "init' first before '" +
            std::string(command) + "'");
  }
  project = *discovered;
}

/// Apply project discovery consistently to all commands that require state.
void discover_project_if_unspecified(
    std::filesystem::path& project, const CLI::Option& project_option,
    const std::string_view command) {
  if (project_option.count() != 0U) {
    return;
  }
  const auto discovered = find_project_root(std::filesystem::current_path());
  if (!discovered) {
    throw kc::vault::VaultError(
        kc::vault::VaultErrorKind::invalid_project,
        "no kc.json found in the current directory or its parents; run 'kc "
        "init' first before '" +
            std::string(command) + "'");
  }
  project = *discovered;
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

  auto* review_command =
      app.add_subcommand("review", "Inspect and correct extracted source evidence");
  review_command->fallthrough();
  review_command->require_subcommand(1);
  auto* review_extraction_command = review_command->add_subcommand(
      "extraction", "Show page image, extracted text, and status together");
  review_extraction_command->fallthrough();
  std::string review_source_id;
  std::uint32_t review_page_number = 0;
  std::string correction_text;
  std::filesystem::path correction_text_file;
  review_extraction_command
      ->add_option("SOURCE_ID", review_source_id, "Imported source ID")
      ->required();
  auto* review_page_option = review_extraction_command->add_option(
      "--page", review_page_number,
      "Show one page, and identify the page when correcting text");
  auto* correction_text_option = review_extraction_command->add_option(
      "--text", correction_text, "Replace the selected page's extracted text");
  auto* correction_file_option = review_extraction_command->add_option(
      "--text-file", correction_text_file,
      "Read replacement text exactly from a UTF-8 file");
  correction_text_option->needs(review_page_option)->excludes(correction_file_option);
  correction_file_option->needs(review_page_option)->excludes(correction_text_option);

  auto* proposal_command =
      app.add_subcommand("proposal", "Review immutable article proposals");
  proposal_command->fallthrough();
  proposal_command->require_subcommand(1);
  auto* proposal_list_command = proposal_command->add_subcommand(
      "list", "List proposals and their review states");
  proposal_list_command->fallthrough();
  std::string proposal_list_status;
  proposal_list_command->add_option(
      "--status", proposal_list_status,
      "Restrict results to pending, approved, rejected, applied, or superseded");

  auto* proposal_show_command = proposal_command->add_subcommand(
      "show", "Inspect proposal sections, citations, and source pages");
  proposal_show_command->fallthrough();
  std::string show_proposal_id;
  proposal_show_command
      ->add_option("PROPOSAL_ID", show_proposal_id, "Proposal ID")
      ->required();

  auto* approve_command = proposal_command->add_subcommand(
      "approve", "Approve a pending proposal without writing the vault");
  approve_command->fallthrough();
  std::string approve_proposal_id;
  approve_command
      ->add_option("PROPOSAL_ID", approve_proposal_id,
                   "Pending proposal ID")
      ->required();

  auto* reject_command = proposal_command->add_subcommand(
      "reject", "Reject a pending proposal without writing the vault");
  reject_command->fallthrough();
  std::string reject_proposal_id;
  std::string rejection_reason;
  reject_command
      ->add_option("PROPOSAL_ID", reject_proposal_id, "Pending proposal ID")
      ->required();
  reject_command->add_option("--reason", rejection_reason,
                             "Optional human review reason");

  auto* apply_command = app.add_subcommand(
      "apply", "Atomically apply an approved proposal to the vault");
  apply_command->fallthrough();
  std::string apply_proposal_id;
  bool allow_overwrite_user_file = false;
  apply_command
      ->add_option("PROPOSAL_ID", apply_proposal_id, "Approved proposal ID")
      ->required();
  apply_command->add_flag(
      "--allow-overwrite-user-file", allow_overwrite_user_file,
      "Explicitly permit replacing an untracked colliding Markdown file");

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

  if (*review_extraction_command) {
    try {
      discover_review_project_if_unspecified(project, *project_option,
                                             "review extraction");
      kc::review::ReviewService reviewer(project);
      std::optional<kc::review::PageCorrectionResult> correction;
      const auto has_inline_correction = correction_text_option->count() != 0U;
      const auto has_file_correction = correction_file_option->count() != 0U;
      if (has_inline_correction || has_file_correction) {
        auto text = has_file_correction
                        ? read_correction_file(correction_text_file)
                        : correction_text;
        correction = reviewer.correct_page_text(
            kc::domain::SourceId{review_source_id}, review_page_number,
            std::move(text));
      }

      const auto review = reviewer.review_extraction(
          kc::domain::SourceId{review_source_id});
      const auto selected_page = review_page_option->count() == 0U
                                     ? std::optional<std::uint32_t>{}
                                     : std::optional<std::uint32_t>{
                                           review_page_number};
      if (selected_page &&
          std::ranges::none_of(review.pages, [&](const auto& page) {
            return page.page_number == *selected_page;
          })) {
        throw kc::review::ReviewError(
            kc::review::ReviewErrorKind::page_not_found,
            "source version has no extracted page " +
                std::to_string(*selected_page));
      }

      if (json_output) {
        std::cout
            << extraction_review_success_json(review, correction, selected_page)
                   .dump()
            << '\n';
      } else if (!quiet) {
        std::cout << "Extraction review: " << review.display_name << " ("
                  << review.source_id.value << ")\n";
        if (correction) {
          std::cout << "Corrected page " << correction->page_number
                    << " and marked it reviewed.\n";
        }
        for (const auto& page : review.pages) {
          if (selected_page && page.page_number != *selected_page) {
            continue;
          }
          std::cout << "\nPage " << page.page_number << " ["
                    << kc::domain::to_string(page.text_status) << "]\n"
                    << "Image: "
                    << (page.image_path ? page.image_path->generic_string()
                                        : "unavailable for this source")
                    << "\nExtracted text:\n"
                    << page.text << '\n';
        }
      }
      return 0;
    } catch (const kc::review::ReviewError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("review extraction", review_error_code(kind),
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc review extraction: " << error.what() << '\n';
      }
      return review_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("review extraction", "internal_error",
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc review extraction: unexpected error: "
                  << error.what() << '\n';
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

  if (*proposal_list_command) {
    try {
      discover_review_project_if_unspecified(project, *project_option,
                                             "proposal list");
      std::optional<kc::domain::ProposalStatus> status;
      if (!proposal_list_status.empty()) {
        status = parse_proposal_status(proposal_list_status);
      }
      kc::review::ReviewService reviewer(project);
      const auto proposals = reviewer.list_proposals(status);
      if (json_output) {
        std::cout << proposal_list_success_json(proposals).dump() << '\n';
      } else if (!quiet) {
        if (proposals.empty()) {
          std::cout << "No proposals found.\n";
        }
        for (const auto& proposal : proposals) {
          std::cout << proposal.proposal_id.value << "  "
                    << kc::domain::to_string(proposal.status) << "  "
                    << kc::domain::to_string(proposal.operation) << "  "
                    << proposal.title << '\n';
        }
      }
      return 0;
    } catch (const kc::review::ReviewError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("proposal list", review_error_code(kind),
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal list: " << error.what() << '\n';
      }
      return review_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("proposal list", "internal_error", error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal list: unexpected error: " << error.what()
                  << '\n';
      }
      return 6;
    }
  }

  if (*proposal_show_command) {
    try {
      discover_review_project_if_unspecified(project, *project_option,
                                             "proposal show");
      kc::review::ReviewService reviewer(project);
      const auto review = reviewer.review_proposal(
          kc::domain::ProposalId{show_proposal_id});
      if (json_output) {
        std::cout << proposal_review_success_json(review).dump() << '\n';
      } else if (!quiet) {
        std::cout << "Proposal " << review.summary.proposal_id.value << " ["
                  << kc::domain::to_string(review.summary.status) << "]\n"
                  << "Operation: "
                  << kc::domain::to_string(review.summary.operation) << "\n\n"
                  << "# " << review.proposal.article.title << '\n';
        for (const auto& section : review.proposal.sections) {
          std::cout << "\n## " << section.heading << '\n';
          for (const auto& block : section.blocks) {
            std::cout << (block.kind == kc::domain::BlockKind::bullet ? "- " : "")
                      << block.text;
            for (const auto& citation : block.citations) {
              std::cout << " [" << citation.page_id.value << ']';
            }
            std::cout << '\n';
          }
        }
        if (!review.proposal.related_concepts.empty()) {
          std::cout << "\n## Related concepts\n";
          for (const auto& related : review.proposal.related_concepts) {
            std::cout << "- [[" << related.title << "]] — "
                      << related.reason << '\n';
          }
        }
        std::cout << "\nCitation evidence\n";
        for (const auto& evidence : review.citation_evidence) {
          std::cout << "\n" << evidence.section_key << " block "
                    << evidence.block_index << ": \""
                    << evidence.citation.quote << "\"\n"
                    << "Source: " << evidence.source_name << ", page "
                    << evidence.page_number << " ["
                    << kc::domain::to_string(evidence.text_status) << "]\n"
                    << "Image: "
                    << (evidence.image_path
                            ? evidence.image_path->generic_string()
                            : "unavailable for this source")
                    << "\nExtracted text:\n"
                    << evidence.extracted_text << '\n';
        }
        std::cout << "\nReview only: no vault article was changed.\n";
      }
      return 0;
    } catch (const kc::review::ReviewError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("proposal show", review_error_code(kind),
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal show: " << error.what() << '\n';
      }
      return review_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("proposal show", "internal_error", error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal show: unexpected error: " << error.what()
                  << '\n';
      }
      return 6;
    }
  }

  if (*approve_command) {
    try {
      discover_project_if_unspecified(project, *project_option,
                                      "proposal approve");
      const kc::domain::ProposalId proposal_id{approve_proposal_id};
      kc::vault::VaultWriter writer(project);
      writer.approve(proposal_id);
      if (json_output) {
        std::cout << approval_success_json(proposal_id).dump() << '\n';
      } else if (!quiet) {
        std::cout << "Approved proposal " << proposal_id.value
                  << "; the vault is unchanged\n";
      }
      return 0;
    } catch (const kc::vault::VaultError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("proposal approve", vault_error_code(kind),
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal approve: " << error.what() << '\n';
      }
      return vault_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("proposal approve", "internal_error",
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal approve: unexpected error: " << error.what()
                  << '\n';
      }
      return 6;
    }
  }

  if (*reject_command) {
    try {
      discover_review_project_if_unspecified(project, *project_option,
                                             "proposal reject");
      const kc::domain::ProposalId proposal_id{reject_proposal_id};
      std::optional<std::string> reason;
      if (!rejection_reason.empty()) {
        reason = rejection_reason;
      }
      kc::review::ReviewService reviewer(project);
      reviewer.reject_proposal(proposal_id, reason);
      if (json_output) {
        std::cout << rejection_success_json(proposal_id, reason).dump() << '\n';
      } else if (!quiet) {
        std::cout << "Rejected proposal " << proposal_id.value
                  << "; the vault is unchanged\n";
      }
      return 0;
    } catch (const kc::review::ReviewError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("proposal reject", review_error_code(kind),
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal reject: " << error.what() << '\n';
      }
      return review_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("proposal reject", "internal_error",
                                error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc proposal reject: unexpected error: " << error.what()
                  << '\n';
      }
      return 6;
    }
  }

  if (*apply_command) {
    try {
      discover_project_if_unspecified(project, *project_option, "apply");
      kc::vault::VaultWriter writer(project);
      const auto result = writer.apply(
          kc::domain::ProposalId{apply_proposal_id},
          {.allow_overwrite_user_file = allow_overwrite_user_file});
      if (json_output) {
        std::cout << apply_success_json(result).dump() << '\n';
      } else if (!quiet) {
        std::cout << "Applied proposal " << result.proposal_id.value << " to "
                  << result.vault_path.generic_string() << '\n';
        if (result.backup_path) {
          std::cout << "Backed up the previous article at "
                    << result.backup_path->generic_string() << '\n';
        }
      }
      return 0;
    } catch (const kc::vault::VaultError& error) {
      const auto kind = error.kind();
      if (json_output) {
        std::cout << error_json("apply", vault_error_code(kind), error.what())
                         .dump()
                  << '\n';
      } else {
        std::cerr << "kc apply: " << error.what() << '\n';
      }
      return vault_exit_code(kind);
    } catch (const std::exception& error) {
      if (json_output) {
        std::cout << error_json("apply", "internal_error", error.what()).dump()
                  << '\n';
      } else {
        std::cerr << "kc apply: unexpected error: " << error.what() << '\n';
      }
      return 6;
    }
  }

  return 2;
}
