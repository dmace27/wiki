#include "kc/import/source_importer.hpp"

#include "kc/domain/text.hpp"
#include "kc/domain/path.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/json.hpp"
#include "kc/import/sha256.hpp"
#include "kc/storage/database.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kc::source_import {
namespace {

struct SourceFormat {
  domain::SourceKind kind;
  std::string_view media_type;
};

class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql) : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
      throw ImportError(ImportErrorKind::state_error,
                        "failed to prepare source state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind_text(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
      throw ImportError(ImportErrorKind::state_error,
                        "failed to bind source state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw ImportError(ImportErrorKind::state_error,
                        "failed to bind source state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw ImportError(ImportErrorKind::state_error,
                        "source state query failed: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
    return status;
  }

  [[nodiscard]] std::string text(const int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value == nullptr ? std::string{}
                            : std::string(reinterpret_cast<const char*>(value),
                             static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)));
  }

  [[nodiscard]] std::int64_t integer(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

 private:
  sqlite3* connection_;
  sqlite3_stmt* statement_{nullptr};
};

class StagedFile {
 public:
  explicit StagedFile(std::filesystem::path path) : path_(std::move(path)) {}
  ~StagedFile() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }
  StagedFile(const StagedFile&) = delete;
  StagedFile& operator=(const StagedFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  void release() noexcept { path_.clear(); }

 private:
  std::filesystem::path path_;
};

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

std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

SourceFormat source_format(const std::filesystem::path& input) {
  const auto extension = lowercase(input.extension().string());
  if (extension == ".pdf") {
    return {domain::SourceKind::pdf, "application/pdf"};
  }
  if (extension == ".md" || extension == ".markdown") {
    return {domain::SourceKind::markdown, "text/markdown"};
  }
  if (extension == ".txt") {
    return {domain::SourceKind::text, "text/plain"};
  }
  throw ImportError(ImportErrorKind::invalid_input,
                    "unsupported source type '" + input.extension().string() +
                        "'; expected .md, .markdown, .txt, or .pdf");
}

bool is_safe_relative(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component == "..";
  });
}

/// Resolve containment again for services reused after directories change.
std::filesystem::path checked_state_path(const std::filesystem::path& root,
                                         const domain::ProjectConfig& config) {
  try {
    domain::validate_project_paths(root, config);
  } catch (const std::invalid_argument& error) {
    throw ImportError(ImportErrorKind::invalid_project, error.what());
  }
  return root / config.paths.state;
}

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  if (!domain::is_within(root, root / "kc.json")) {
    throw ImportError(ImportErrorKind::invalid_project, "configuration escapes project root");
  }
  const auto config_path = root / "kc.json";
  std::ifstream input(config_path);
  if (!input) {
    throw ImportError(ImportErrorKind::invalid_project,
                      "no readable kc.json at project root: " + root.string());
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      const auto detail = parsed.issues.empty() ? "unknown validation error"
                                                : parsed.issues.front().message;
      throw ImportError(ImportErrorKind::invalid_project,
                        "invalid project configuration: " + detail);
    }
    try {
      domain::validate_project_paths(root, *parsed.value);
    } catch (const std::invalid_argument& error) {
      throw ImportError(ImportErrorKind::invalid_project, error.what());
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception& error) {
    throw ImportError(ImportErrorKind::invalid_project,
                      "invalid project configuration JSON: " +
                          std::string(error.what()));
  }
}

void make_read_only(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
          std::filesystem::perms::others_read,
      std::filesystem::perm_options::replace, error);
  if (error) {
    throw ImportError(ImportErrorKind::unavailable_input,
                      "could not make retained source read-only: " + error.message());
  }
}

domain::SourceKind parse_kind(const std::string_view value) {
  if (value == "pdf") {
    return domain::SourceKind::pdf;
  }
  if (value == "markdown") {
    return domain::SourceKind::markdown;
  }
  if (value == "text") {
    return domain::SourceKind::text;
  }
  throw ImportError(ImportErrorKind::state_error,
                    "project database contains an unknown source kind");
}

/// Global lookups reuse bytes; source-scoped lookups detect idempotence and
/// historical restoration without borrowing another document's identity.
std::optional<ImportResult> find_version_by_hash(
    sqlite3* connection, const std::string_view hash,
    const std::optional<domain::SourceId>& source_id = std::nullopt) {
  Statement statement(
      connection,
      "SELECT s.source_id, s.display_name, s.source_kind, s.created_at, "
      "s.archived_at, v.source_version_id, v.sha256, v.original_filename, "
      "v.stored_path, v.media_type, v.byte_size, v.imported_at "
      "FROM source_versions v JOIN sources s ON s.source_id = v.source_id "
      "WHERE v.sha256 = ? AND (? IS NULL OR s.source_id = ?) "
      "ORDER BY v.rowid DESC LIMIT 1");
  statement.bind_text(1, hash);
  if (source_id) {
    statement.bind_text(2, source_id->value);
    statement.bind_text(3, source_id->value);
  }
  if (statement.step() != SQLITE_ROW) {
    return std::nullopt;
  }

  domain::Source source{
      .source_id = {statement.text(0)},
      .display_name = statement.text(1),
      .source_kind = parse_kind(statement.text(2)),
      .created_at = statement.text(3)};
  if (sqlite3_column_type(statement.get(), 4) != SQLITE_NULL) {
    source.archived_at = statement.text(4);
  }
  domain::SourceVersion version{
      .source_version_id = {statement.text(5)},
      .source_id = source.source_id,
      .sha256 = statement.text(6),
      .original_filename = statement.text(7),
      .stored_path = domain::path_from_utf8(statement.text(8)),
      .media_type = statement.text(9),
      .byte_size = static_cast<std::uint64_t>(statement.integer(10)),
      .imported_at = statement.text(11)};
  return ImportResult{.source = std::move(source),
                      .version = std::move(version),
                      .deduplicated = true};
}

std::optional<domain::Source> find_source(sqlite3* connection,
                                         const domain::SourceId& source_id) {
  Statement statement(
      connection,
      "SELECT source_id, display_name, source_kind, created_at, archived_at "
      "FROM sources WHERE source_id = ?");
  statement.bind_text(1, source_id.value);
  if (statement.step() != SQLITE_ROW) {
    return std::nullopt;
  }
  domain::Source source{
      .source_id = {statement.text(0)},
      .display_name = statement.text(1),
      .source_kind = parse_kind(statement.text(2)),
      .created_at = statement.text(3)};
  if (sqlite3_column_type(statement.get(), 4) != SQLITE_NULL) {
    source.archived_at = statement.text(4);
  }
  return source;
}

void insert_source(sqlite3* connection, const domain::Source& source) {
  Statement statement(
      connection,
      "INSERT INTO sources(source_id, display_name, source_kind, created_at) "
      "VALUES (?, ?, ?, ?)");
  statement.bind_text(1, source.source_id.value);
  statement.bind_text(2, source.display_name);
  statement.bind_text(3, domain::to_string(source.source_kind));
  statement.bind_text(4, source.created_at);
  if (statement.step() != SQLITE_DONE) {
    throw ImportError(ImportErrorKind::state_error, "failed to insert source");
  }
}

void insert_version(sqlite3* connection, const domain::SourceVersion& version) {
  if (version.byte_size >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw ImportError(ImportErrorKind::invalid_input,
                      "source is too large for project state");
  }
  Statement statement(
      connection,
      "INSERT INTO source_versions("
      "source_version_id, source_id, sha256, original_filename, stored_path, "
      "media_type, byte_size, imported_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  statement.bind_text(1, version.source_version_id.value);
  statement.bind_text(2, version.source_id.value);
  statement.bind_text(3, version.sha256);
  statement.bind_text(4, version.original_filename);
  statement.bind_text(5, domain::path_to_utf8(version.stored_path));
  statement.bind_text(6, version.media_type);
  statement.bind_integer(7, static_cast<std::int64_t>(version.byte_size));
  statement.bind_text(8, version.imported_at);
  if (statement.step() != SQLITE_DONE) {
    throw ImportError(ImportErrorKind::state_error, "failed to insert source version");
  }
}

}  // namespace

SourceImporter::SourceImporter(std::filesystem::path project_root)
    : project_root_(std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      config_(read_config(project_root_)) {
  const auto state_path = checked_state_path(project_root_, config_);
  if (!std::filesystem::is_regular_file(state_path)) {
    throw ImportError(ImportErrorKind::invalid_project,
                      "project state database does not exist; run 'kc init' first");
  }
  storage::Database database(state_path);
  if (database.scalar_integer(
          "SELECT COUNT(*) FROM schema_migrations WHERE version = 3") != 1) {
    throw ImportError(ImportErrorKind::invalid_project,
                      "source identity migration is required; run 'kc init' first");
  }
}

ImportResult SourceImporter::import_file(const std::filesystem::path& input_path,
                                        const ImportOptions& options) {
  // Revalidate even when callers reuse an importer after editing directories.
  static_cast<void>(read_config(project_root_));
  const auto format = source_format(input_path);
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(input_path, filesystem_error)) {
    throw ImportError(ImportErrorKind::unavailable_input,
                      "source is not a readable regular file: " + input_path.string());
  }

  const auto staging_directory =
      project_root_ / config_.paths.cache / "import-staging";
  if (!domain::is_within(project_root_ / config_.paths.cache, staging_directory)) {
    throw ImportError(ImportErrorKind::invalid_project, "unsafe import staging path");
  }
  try {
    std::filesystem::create_directories(staging_directory);
  } catch (const std::filesystem::filesystem_error& error) {
    throw ImportError(ImportErrorKind::unavailable_input,
                      "could not create import staging directory: " +
                          std::string(error.what()));
  }

  const auto staged_path =
      staging_directory /
      (domain::generate_prefixed_ulid("import_") + ".tmp");
  try {
    if (!std::filesystem::copy_file(
            input_path, staged_path, std::filesystem::copy_options::none)) {
      throw ImportError(ImportErrorKind::unavailable_input,
                        "could not stage source file: " + input_path.string());
    }
  } catch (const std::filesystem::filesystem_error& error) {
    throw ImportError(ImportErrorKind::unavailable_input,
                      "could not read source file: " + std::string(error.what()));
  }
  StagedFile staged(staged_path);

  std::string hash;
  std::uint64_t byte_size = 0;
  try {
    if (format.kind != domain::SourceKind::pdf) {
      std::ifstream input(staged.path(), std::ios::binary);
      const std::string text{std::istreambuf_iterator<char>(input), {}};
      if (!input || !domain::is_valid_source_text(text)) {
        throw ImportError(ImportErrorKind::invalid_input,
                          "text input must be UTF-8 without embedded NULs");
      }
    }
    hash = sha256_file(staged.path());
    byte_size = std::filesystem::file_size(staged.path());
  } catch (const ImportError&) {
    throw;
  } catch (const std::exception& error) {
    throw ImportError(ImportErrorKind::unavailable_input, error.what());
  }

  storage::Database database(checked_state_path(project_root_, config_));
  auto* connection = database.native_handle();
  database.execute("BEGIN IMMEDIATE;");
  std::filesystem::path newly_retained_path;
  try {
    const auto filename = input_path.filename().string();
    std::optional<domain::Source> source;
    if (options.source_id) {
      source = find_source(connection, *options.source_id);
      if (!source || source->archived_at) {
        throw ImportError(ImportErrorKind::invalid_input,
                          "revision source does not exist or is archived: " +
                              options.source_id->value);
      }
      if (source->source_kind != format.kind) {
        throw ImportError(ImportErrorKind::invalid_input,
                          "revision must use the existing source kind");
      }
    }

    // Identity and physical byte reuse are independent. Never return another
    // source's identity just because its file has the same digest.
    auto existing = find_version_by_hash(connection, hash);
    if (existing) {
      if (!is_safe_relative(existing->version.stored_path) ||
          !domain::is_within(project_root_ / config_.paths.sources,
                             project_root_ / existing->version.stored_path)) {
        throw ImportError(ImportErrorKind::state_error,
                          "project database contains an unsafe retained source path");
      }
      const auto retained_path = project_root_ / existing->version.stored_path;
      if (std::filesystem::exists(retained_path)) {
        if (!std::filesystem::is_regular_file(retained_path) ||
            sha256_file(retained_path) != hash) {
          throw ImportError(ImportErrorKind::state_error,
                            "retained source does not match its recorded SHA-256");
        }
      } else {
        // Repair a missing shared copy from the staged, hash-verified input.
        std::filesystem::create_directories(retained_path.parent_path());
        std::filesystem::rename(staged.path(), retained_path);
        staged.release();
        make_read_only(retained_path);
      }
    }

    bool restored = false;
    if (source) {
      Statement latest(connection,
                       "SELECT sha256 FROM source_versions WHERE source_id = ? "
                       "ORDER BY rowid DESC LIMIT 1");
      latest.bind_text(1, source->source_id.value);
      if (latest.step() == SQLITE_ROW && latest.text(0) == hash) {
        auto current = find_version_by_hash(connection, hash, source->source_id);
        database.execute("COMMIT;");
        return *current;
      }
      restored = find_version_by_hash(connection, hash, source->source_id).has_value();
    }

    const auto source_created = !source.has_value();
    if (!source) {
      source = domain::Source{
          .source_id = domain::generate_source_id(),
          .display_name = filename,
          .source_kind = format.kind,
          .created_at = utc_now()};
      insert_source(connection, *source);
    }

    domain::SourceVersion version{
        .source_version_id = domain::generate_source_version_id(),
        .source_id = source->source_id,
        .sha256 = hash,
        .original_filename = filename,
        .media_type = std::string(format.media_type),
        .byte_size = byte_size,
        .imported_at = utc_now()};
    version.stored_path =
        config_.paths.sources / source->source_id.value /
        version.source_version_id.value / filename;
    if (!is_safe_relative(version.stored_path) ||
        !domain::is_within(project_root_ / config_.paths.sources,
                           project_root_ / version.stored_path)) {
      throw ImportError(ImportErrorKind::invalid_project,
                        "configured source directory is not project-relative");
    }

    if (existing) {
      version.stored_path = existing->version.stored_path;
    } else {
      newly_retained_path = project_root_ / version.stored_path;
      std::filesystem::create_directories(newly_retained_path.parent_path());
      if (std::filesystem::exists(newly_retained_path)) {
        throw ImportError(ImportErrorKind::state_error,
                          "generated retained source path already exists");
      }
      std::filesystem::rename(staged.path(), newly_retained_path);
      staged.release();
      make_read_only(newly_retained_path);
    }
    insert_version(connection, version);
    database.execute("COMMIT;");
    return ImportResult{.source = std::move(*source),
                        .version = std::move(version),
                        .source_created = source_created,
                        .version_created = true,
                        .deduplicated = existing.has_value(),
                        .restored = restored};
  } catch (...) {
    try {
      database.execute("ROLLBACK;");
    } catch (...) {
      // Preserve the import error; rollback diagnostics are secondary.
    }
    if (!newly_retained_path.empty()) {
      std::error_code ignored;
      std::filesystem::permissions(
          newly_retained_path, std::filesystem::perms::owner_write,
          std::filesystem::perm_options::add, ignored);
      std::filesystem::remove(newly_retained_path, ignored);
    }
    throw;
  }
}

std::vector<ImportFileResult> SourceImporter::import_files(
    const std::vector<std::filesystem::path>& input_paths) {
  std::vector<ImportFileResult> results;
  results.reserve(input_paths.size());
  for (const auto& input : input_paths) {
    try {
      results.push_back({input, import_file(input), std::nullopt});
    } catch (const ImportError& error) {
      results.push_back({input, std::nullopt, ImportFailure{error.kind(), error.what()}});
    } catch (const std::exception& error) {
      // Storage/filesystem failures also belong to the affected file, so prior
      // durable successes and later inputs remain observable to callers.
      results.push_back({input, std::nullopt,
                         ImportFailure{ImportErrorKind::state_error, error.what()}});
    }
  }
  return results;
}

}  // namespace kc::source_import
