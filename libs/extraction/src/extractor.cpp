#include "kc/extraction/extractor.hpp"

#include "kc/import/integrity.hpp"
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
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kc::extraction {
namespace {

struct SourceRecord {
  domain::SourceId source_id;
  domain::SourceVersionId version_id;
  domain::SourceKind kind;
  std::filesystem::path stored_path;
  std::string sha256;
};

class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(
            connection_, query.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "failed to prepare extraction state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind_text(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "failed to bind extraction state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "failed to bind extraction state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_null(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "failed to bind extraction state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "extraction state query failed: " +
              std::string(sqlite3_errmsg(connection_)));
    }
    return status;
  }

  [[nodiscard]] std::string text(const int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char*>(value),
                             static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)));
  }

  [[nodiscard]] std::int64_t integer(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] bool is_null(const int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

 private:
  sqlite3* connection_;
  sqlite3_stmt* statement_{nullptr};
};

class StagingDirectory {
 public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {
    std::filesystem::create_directories(path_);
  }

  ~StagingDirectory() {
    std::error_code ignored;
    if (!path_.empty()) std::filesystem::remove_all(path_, ignored);
  }

  void release() noexcept { path_.clear(); }

  StagingDirectory(const StagingDirectory&) = delete;
  StagingDirectory& operator=(const StagingDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

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
    throw ExtractionError(ExtractionErrorKind::invalid_project, error.what());
  }
  return root / config.paths.state;
}

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  if (!domain::is_within(root, root / "kc.json")) {
    throw ExtractionError(ExtractionErrorKind::invalid_project, "configuration escapes project root");
  }
  const auto config_path = root / "kc.json";
  std::ifstream input(config_path);
  if (!input) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "no readable kc.json at project root: " + root.string());
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      const auto detail =
          parsed.issues.empty() ? "unknown validation error"
                                : parsed.issues.front().message;
      throw ExtractionError(
          ExtractionErrorKind::invalid_project,
          "invalid project configuration: " + detail);
    }
    try {
      domain::validate_project_paths(root, *parsed.value);
    } catch (const std::invalid_argument& error) {
      throw ExtractionError(ExtractionErrorKind::invalid_project, error.what());
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception& error) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "invalid project configuration JSON: " + std::string(error.what()));
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
  throw ExtractionError(
      ExtractionErrorKind::state_error,
      "project database contains an unknown source kind");
}

SourceRecord find_latest_source_version(
    sqlite3* connection, const domain::SourceId& source_id) {
  Statement statement(
      connection,
      "SELECT s.source_id, v.source_version_id, s.source_kind, v.stored_path "
      " , v.sha256 FROM sources s JOIN source_versions v ON v.source_id = s.source_id "
      "WHERE s.source_id = ? AND s.archived_at IS NULL "
      "ORDER BY v.rowid DESC LIMIT 1");
  statement.bind_text(1, source_id.value);
  if (statement.step() != SQLITE_ROW) {
    throw ExtractionError(
        ExtractionErrorKind::source_not_found,
        "source does not exist, is archived, or has no imported version: " +
            source_id.value);
  }
  SourceRecord result{
      .source_id = {statement.text(0)},
      .version_id = {statement.text(1)},
      .kind = parse_kind(statement.text(2)),
      .stored_path = domain::path_from_utf8(statement.text(3)),
      .sha256 = statement.text(4)};
  if (!is_safe_relative(result.stored_path)) {
    throw ExtractionError(
        ExtractionErrorKind::state_error,
        "project database contains an unsafe retained source path");
  }
  return result;
}

/// Compare complete durable page state, not only citation substrings.
std::string page_snapshot(sqlite3* connection,
                          const domain::SourceVersionId& version_id) {
  Statement query(connection,
      "SELECT page_id, page_number, image_path, text, text_status, text_sha256, "
      "evidence_revision, image_sha256 FROM source_pages "
      "WHERE source_version_id = ? ORDER BY page_number");
  query.bind_text(1, version_id.value);
  auto rows = nlohmann::json::array();
  while (query.step() == SQLITE_ROW) {
    rows.push_back({query.text(0), query.integer(1), query.text(2),
                    query.text(3), query.text(4), query.text(5),
                    query.integer(6), query.text(7)});
  }
  return rows.dump();
}

std::vector<domain::ExtractedPage> read_existing_pages(
    sqlite3* connection, const domain::SourceVersionId& version_id,
    const std::filesystem::path& root, const domain::ProjectConfig& config,
    const bool verify_files) {
  Statement statement(
      connection,
      "SELECT page_id, page_number, image_path, text, text_status, text_sha256, image_sha256 "
      "FROM source_pages WHERE source_version_id = ? ORDER BY page_number");
  statement.bind_text(1, version_id.value);
  std::vector<domain::ExtractedPage> pages;
  while (statement.step() == SQLITE_ROW) {
    domain::ExtractedPage page{
        .page_id = {statement.text(0)},
        .source_version_id = version_id,
        .page_number =
            static_cast<std::uint32_t>(statement.integer(1)),
        .text = statement.text(3)};
    if (!statement.is_null(2)) {
      page.image_path = domain::path_from_utf8(statement.text(2));
      if (!is_safe_relative(*page.image_path)) {
        throw ExtractionError(
            ExtractionErrorKind::state_error,
            "project database contains an unsafe rendered page path");
      }
    }
    if (!domain::is_valid_source_text(page.text) ||
        source_import::sha256_text(page.text) != statement.text(5)) {
      throw ExtractionError(ExtractionErrorKind::invalid_state,
                            "cached page text has invalid encoding or SHA-256");
    }
    if (verify_files && page.image_path) {
      source_import::verify_retained_file(root / config.paths.cache,
          root / *page.image_path, statement.text(6));
    }
    const auto status = statement.text(4);
    if (status == "native") {
      page.text_status = domain::TextStatus::native;
    } else if (status == "ocr_unreviewed") {
      page.text_status = domain::TextStatus::ocr_unreviewed;
    } else if (status == "reviewed") {
      page.text_status = domain::TextStatus::reviewed;
    } else if (status == "failed") {
      page.text_status = domain::TextStatus::failed;
    } else {
      throw ExtractionError(
          ExtractionErrorKind::state_error,
          "project database contains an unknown page text status");
    }
    pages.push_back(std::move(page));
  }
  return pages;
}

/// Refuse to replace extraction state once a proposal captures citations or
/// generation context. This mirrors the review correction boundary.
void ensure_pages_are_not_cited(
    sqlite3* connection, const domain::SourceVersionId& version_id) {
  Statement statement(
      connection,
      "SELECT p.page_id FROM source_pages p "
      "WHERE p.source_version_id = ? AND (EXISTS ("
      "SELECT 1 FROM proposal_citations pc WHERE pc.page_id = p.page_id) "
      "OR EXISTS (SELECT 1 FROM proposal_evidence pe WHERE pe.page_id = p.page_id)) "
      "ORDER BY p.page_number LIMIT 1");
  statement.bind_text(1, version_id.value);
  if (statement.step() == SQLITE_ROW) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_state,
        "source cannot be re-extracted after a proposal cites page " +
            statement.text(0));
  }
}

std::map<std::uint32_t, domain::PageId> page_ids_by_number(
    const std::vector<domain::ExtractedPage>& pages) {
  std::map<std::uint32_t, domain::PageId> result;
  for (const auto& page : pages) {
    result.emplace(page.page_number, page.page_id);
  }
  return result;
}

std::size_t count_failed(
    const std::vector<domain::ExtractedPage>& pages) {
  return static_cast<std::size_t>(std::ranges::count_if(
      pages, [](const domain::ExtractedPage& page) {
        return page.text_status == domain::TextStatus::failed;
      }));
}

std::string read_source_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw ExtractionError(
        ExtractionErrorKind::source_not_found,
        "retained source file is missing or unreadable");
  }
  std::string text{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw ExtractionError(
        ExtractionErrorKind::source_not_found,
        "retained source file could not be read completely");
  }
  return text;
}

/// Reject whitespace-only or tiny extraction artifacts before skipping OCR.
bool has_usable_native_text(const std::string_view text) {
  std::size_t visible = 0;
  std::size_t substantive = 0;
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (!std::isspace(byte)) {
      ++visible;
    }
    if (std::isalnum(byte) || byte >= 0x80U) {
      ++substantive;
    }
  }
  return visible >= 4U && substantive >= 2U;
}

/// OCR is already untrusted and review-gated, so retain even short nonblank
/// results instead of discarding legitimate page labels or formula fragments.
bool has_visible_text(const std::string_view text) {
  return std::ranges::any_of(text, [](const char character) {
    return !std::isspace(static_cast<unsigned char>(character));
  });
}

void insert_run(sqlite3* connection, const domain::RunId& run_id,
                const SourceRecord& source, const std::string_view extractor,
                const std::optional<std::string_view> ocr_provider) {
  Statement statement(
      connection,
      "INSERT INTO extraction_runs("
      "run_id, source_version_id, extractor_name, extractor_version, "
      "ocr_provider, status, started_at) VALUES (?, ?, ?, NULL, ?, 'running', ?)");
  statement.bind_text(1, run_id.value);
  statement.bind_text(2, source.version_id.value);
  statement.bind_text(3, extractor);
  if (ocr_provider) {
    statement.bind_text(4, *ocr_provider);
  } else {
    statement.bind_null(4);
  }
  statement.bind_text(5, utc_now());
  static_cast<void>(statement.step());
}

void update_run(sqlite3* connection, const domain::RunId& run_id,
                const bool failed, const std::string_view error_message) {
  Statement statement(
      connection,
      "UPDATE extraction_runs SET status = ?, completed_at = ?, "
      "error_message = ? WHERE run_id = ?");
  statement.bind_text(1, failed ? "failed" : "completed");
  statement.bind_text(2, utc_now());
  if (error_message.empty()) {
    statement.bind_null(3);
  } else {
    statement.bind_text(3, error_message);
  }
  statement.bind_text(4, run_id.value);
  static_cast<void>(statement.step());
}

void mark_run_failed(
    storage::Database& database, const domain::RunId& run_id,
    const std::string_view message) noexcept {
  try {
    update_run(database.native_handle(), run_id, true, message);
  } catch (...) {
    // Preserve the extraction failure that caused this diagnostic update.
  }
}

void upsert_page(sqlite3* connection,
                 const domain::ExtractedPage& page,
                 const std::filesystem::path& root) {
  Statement statement(
      connection,
      "INSERT INTO source_pages("
      "page_id, source_version_id, page_number, image_path, text, "
      "text_status, text_sha256, image_sha256) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(source_version_id, page_number) DO UPDATE SET "
      "image_path = excluded.image_path, text = excluded.text, "
      "text_status = excluded.text_status, text_sha256 = excluded.text_sha256, "
      "image_sha256 = excluded.image_sha256, "
      "evidence_revision = source_pages.evidence_revision + 1");
  statement.bind_text(1, page.page_id.value);
  statement.bind_text(2, page.source_version_id.value);
  statement.bind_integer(3, static_cast<std::int64_t>(page.page_number));
  if (page.image_path) {
    statement.bind_text(4, domain::path_to_utf8(*page.image_path));
  } else {
    statement.bind_null(4);
  }
  statement.bind_text(5, page.text);
  statement.bind_text(6, domain::to_string(page.text_status));
  statement.bind_text(7, source_import::sha256_text(page.text));
  if (page.image_path) {
    statement.bind_text(8, source_import::sha256_file(root / *page.image_path));
  } else {
    statement.bind_null(8);
  }
  static_cast<void>(statement.step());
}

void persist_result(storage::Database& database,
                    const domain::RunId& run_id,
                    const domain::SourceVersionId& version_id,
                    const std::string& expected_snapshot,
                    const std::filesystem::path& root,
                    const std::vector<domain::ExtractedPage>& pages,
                    const std::size_t failed_pages) {
  database.execute("BEGIN IMMEDIATE;");
  try {
    // Repeat the citation check under the write lock. A compiler process may
    // have created a proposal while rendering or OCR was still in progress.
    if (page_snapshot(database.native_handle(), version_id) != expected_snapshot) {
      throw ExtractionError(ExtractionErrorKind::invalid_state,
                            "page evidence changed during extraction; retry");
    }
    ensure_pages_are_not_cited(database.native_handle(), version_id);
    // Removed pages must not survive a shorter re-extraction.
    Statement remove(database.native_handle(),
        "DELETE FROM source_pages WHERE source_version_id = ? AND page_number > ?");
    remove.bind_text(1, version_id.value);
    remove.bind_integer(2, static_cast<std::int64_t>(pages.size()));
    static_cast<void>(remove.step());
    for (const auto& page : pages) {
      upsert_page(database.native_handle(), page, root);
    }
    const auto message =
        failed_pages == 0U
            ? std::string{}
            : "OCR failed or produced unusable text for " +
                  std::to_string(failed_pages) + " page(s)";
    update_run(
        database.native_handle(), run_id, failed_pages != 0U, message);
    database.execute("COMMIT;");
  } catch (...) {
    try {
      database.execute("ROLLBACK;");
    } catch (...) {
      // Preserve the page persistence error.
    }
    throw;
  }
}

std::string padded_page_number(const std::uint32_t page_number) {
  std::ostringstream output;
  output << std::setw(4) << std::setfill('0') << page_number;
  return output.str();
}

}  // namespace

Extractor::Extractor(std::filesystem::path project_root)
    : project_root_(
          std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      config_(read_config(project_root_)),
      pdf_adapter_(std::make_unique<PopplerPdfAdapter>()),
      ocr_provider_(config_.providers.ocr.default_provider == "tesseract"
                        ? std::make_unique<TesseractOcrProvider>(
                              config_.providers.ocr.language)
                        : nullptr) {
  if (!std::filesystem::is_regular_file(
          checked_state_path(project_root_, config_))) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "project state database does not exist; run 'kc init' first");
  }
  if (!ocr_provider_) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "unsupported local OCR provider: " +
            config_.providers.ocr.default_provider);
  }
}

Extractor::Extractor(std::filesystem::path project_root,
                     std::unique_ptr<PdfAdapter> pdf_adapter,
                     std::unique_ptr<OcrProvider> ocr_provider)
    : project_root_(
          std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      config_(read_config(project_root_)),
      pdf_adapter_(std::move(pdf_adapter)),
      ocr_provider_(std::move(ocr_provider)) {
  if (!std::filesystem::is_regular_file(
          checked_state_path(project_root_, config_))) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "project state database does not exist; run 'kc init' first");
  }
  if (!pdf_adapter_ || !ocr_provider_) {
    throw ExtractionError(
        ExtractionErrorKind::invalid_project,
        "extraction adapters must not be null");
  }
}

ExtractionResult Extractor::extract(
    const domain::SourceId& source_id, const bool force) {
  static_cast<void>(read_config(project_root_));
  storage::Database database(checked_state_path(project_root_, config_));
  // Both readers share one SQLite snapshot; no lock is held across adapters.
  database.execute("BEGIN;");
  const auto source = find_latest_source_version(database.native_handle(), source_id);
  const auto retained_path = project_root_ / source.stored_path;
  std::vector<domain::ExtractedPage> existing;
  std::string expected_snapshot;
  try {
    source_import::verify_retained_file(project_root_ / config_.paths.sources,
                                      retained_path, source.sha256);
    existing = read_existing_pages(database.native_handle(), source.version_id,
                                   project_root_, config_, !force);
    expected_snapshot = page_snapshot(database.native_handle(), source.version_id);
    database.execute("COMMIT;");
  } catch (const std::exception& error) {
    database.execute("ROLLBACK;");
    throw ExtractionError(ExtractionErrorKind::invalid_state, error.what());
  }
  if (!force && !existing.empty()) {
    return {
        .source_id = source.source_id,
        .source_version_id = source.version_id,
        .pages = existing,
        .reused = true,
        .failed_pages = count_failed(existing)};
  }
  if (force && !existing.empty()) {
    // Fail before invoking external adapters when immutable proposal evidence
    // already makes this source version ineligible for replacement.
    ensure_pages_are_not_cited(database.native_handle(), source.version_id);
  }
  const auto existing_ids = page_ids_by_number(existing);

  const auto run_id = domain::generate_run_id();
  const auto extractor_name =
      source.kind == domain::SourceKind::pdf
          ? pdf_adapter_->name()
          : std::string_view("plain-text");
  const auto ocr_name =
      source.kind == domain::SourceKind::pdf
          ? std::optional<std::string_view>(ocr_provider_->name())
          : std::nullopt;
  insert_run(
      database.native_handle(), run_id, source, extractor_name, ocr_name);

  const auto output_directory = config_.paths.cache / "pages" /
                                source.version_id.value / run_id.value;
  try {
    if (!domain::is_within(project_root_ / config_.paths.cache,
                           project_root_ / output_directory)) {
      throw ExtractionError(ExtractionErrorKind::invalid_project, "unsafe page output path");
    }
    // Each attempt owns its images. Failed or stale attempts remove only their
    // own directory; previously published images are never overwritten.
    StagingDirectory output(project_root_ / output_directory);
    std::vector<domain::ExtractedPage> pages;
    if (source.kind != domain::SourceKind::pdf) {
      const auto text = read_source_text(retained_path);
      if (!domain::is_valid_source_text(text)) {
        throw ExtractionError(ExtractionErrorKind::invalid_state,
                              "source text must be UTF-8 without embedded NULs");
      }
      if (source_import::sha256_text(text) != source.sha256) {
        throw ExtractionError(ExtractionErrorKind::invalid_state,
                              "source bytes changed while reading text");
      }
      const auto existing_id = existing_ids.find(1U);
      pages.push_back({
          .page_id = existing_id == existing_ids.end()
                         ? domain::generate_page_id()
                         : existing_id->second,
          .source_version_id = source.version_id,
          .page_number = 1,
          .text = text,
          .text_status = domain::TextStatus::native});
    } else {
      const auto staging_path =
          project_root_ / config_.paths.cache / "extraction-staging" /
          run_id.value;
      if (!domain::is_within(project_root_ / config_.paths.cache, staging_path)) {
        throw ExtractionError(ExtractionErrorKind::invalid_project, "unsafe extraction staging path");
      }
      StagingDirectory staging(staging_path);
      // Adapters consume a verified private snapshot, so edits of the retained
      // source during rendering cannot mix bytes from different revisions.
      const auto adapter_source = staging.path() / "source.pdf";
      std::filesystem::copy_file(retained_path, adapter_source);
      source_import::verify_retained_file(staging.path(), adapter_source, source.sha256);
      const auto rendered_pages =
          pdf_adapter_->render_pages(adapter_source, staging.path());
      if (rendered_pages.empty()) {
        throw ExtractionError(ExtractionErrorKind::adapter_error, "renderer returned no pages");
      }
      pages.reserve(rendered_pages.size());
      const auto& image_directory = output_directory;

      for (const auto& rendered : rendered_pages) {
        const auto existing_id =
            existing_ids.find(rendered.page_number);
        domain::ExtractedPage page{
            .page_id = existing_id == existing_ids.end()
                           ? domain::generate_page_id()
                           : existing_id->second,
            .source_version_id = source.version_id,
            .page_number = rendered.page_number,
            .image_path =
                image_directory /
                ("page-" + padded_page_number(rendered.page_number) + ".png"),
            .text_status = domain::TextStatus::failed};

        if (rendered.page_number != pages.size() + 1U ||
            !domain::is_within(staging.path(), rendered.image_path) ||
            !std::filesystem::is_regular_file(rendered.image_path)) {
          throw ExtractionError(ExtractionErrorKind::adapter_error,
                                "renderer returned invalid page order or image path");
        }
        const auto final_image = project_root_ / *page.image_path;
        std::filesystem::copy_file(
            rendered.image_path, final_image,
            std::filesystem::copy_options::none);

        std::optional<std::string> native_text;
        try {
          native_text = pdf_adapter_->extract_native_text(
              adapter_source, rendered.page_number, staging.path());
        } catch (const AdapterError&) {
          // Native extraction is opportunistic. A rendered page can still be
          // recovered by OCR, so an adapter error here is page-local.
        }
        if (native_text && !domain::is_valid_source_text(*native_text)) {
          throw ExtractionError(ExtractionErrorKind::adapter_error,
                                "native extraction must be UTF-8 without embedded NULs");
        }
        if (native_text && has_usable_native_text(*native_text)) {
          page.text = *native_text;
          page.text_status = domain::TextStatus::native;
        } else {
          try {
            const auto ocr = ocr_provider_->recognize(rendered.image_path);
            if (!domain::is_valid_source_text(ocr.text)) {
              throw ExtractionError(ExtractionErrorKind::adapter_error,
                                    "OCR text must be UTF-8 without embedded NULs");
            }
            if (ocr.succeeded && has_visible_text(ocr.text)) {
              page.text = ocr.text;
              page.text_status = domain::TextStatus::ocr_unreviewed;
            }
          } catch (const AdapterError&) {
            // A failed OCR adapter must still leave a durable failed page row
            // beside its rendered evidence.
          }
        }
        pages.push_back(std::move(page));
      }
      source_import::verify_retained_file(staging.path(), adapter_source, source.sha256);
    }

    const auto failed_pages = count_failed(pages);
    source_import::verify_retained_file(project_root_ / config_.paths.sources,
                                      retained_path, source.sha256);
    persist_result(database, run_id, source.version_id, expected_snapshot,
                   project_root_, pages, failed_pages);
    output.release();
    return {
        .source_id = source.source_id,
        .source_version_id = source.version_id,
        .run_id = run_id,
        .pages = std::move(pages),
        .failed_pages = failed_pages};
  } catch (const AdapterError& error) {
    mark_run_failed(database, run_id, "PDF or OCR adapter failed");
    throw ExtractionError(
        ExtractionErrorKind::adapter_error, error.what());
  } catch (const ExtractionError& error) {
    mark_run_failed(database, run_id, "extraction failed");
    throw;
  } catch (const std::filesystem::filesystem_error& error) {
    mark_run_failed(database, run_id, "could not retain extracted page output");
    throw ExtractionError(
        ExtractionErrorKind::adapter_error,
        "could not retain extracted page output: " +
            std::string(error.what()));
  } catch (const std::exception& error) {
    mark_run_failed(database, run_id, "unexpected extraction failure");
    throw ExtractionError(
        ExtractionErrorKind::state_error,
        "unexpected extraction failure: " + std::string(error.what()));
  }
}

}  // namespace kc::extraction
