#include "kc/review/review_service.hpp"

#include "kc/domain/json.hpp"
#include "kc/import/sha256.hpp"
#include "kc/storage/database.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace kc::review {
namespace {

/// RAII wrapper for parameterized review queries.
class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_,
                           nullptr) != SQLITE_OK) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "failed to prepare review state query: " +
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
      throw ReviewError(ReviewErrorKind::state_error,
                        "failed to bind review state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_null(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "failed to bind review state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "failed to bind review state query: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "review state query failed: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
    return status;
  }

  [[nodiscard]] std::string text(const int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char*>(value));
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

/// Keep page correction and its hash/status transition atomic.
class Transaction {
 public:
  explicit Transaction(storage::Database& database) : database_(database) {
    database_.execute("BEGIN IMMEDIATE;");
  }

  ~Transaction() {
    if (!committed_) {
      try {
        database_.execute("ROLLBACK;");
      } catch (...) {
        // Preserve the primary review error.
      }
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void commit() {
    database_.execute("COMMIT;");
    committed_ = true;
  }

 private:
  storage::Database& database_;
  bool committed_{false};
};

struct SourceRecord {
  domain::SourceId source_id;
  domain::SourceVersionId version_id;
  std::string display_name;
  domain::SourceKind source_kind{domain::SourceKind::text};
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

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  std::ifstream input(root / "kc.json");
  if (!input) {
    throw ReviewError(ReviewErrorKind::invalid_project,
                      "no readable kc.json at project root");
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      const auto detail = parsed.issues.empty()
                              ? "unknown validation error"
                              : parsed.issues.front().message;
      throw ReviewError(ReviewErrorKind::invalid_project,
                        "invalid project configuration: " + detail);
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    throw ReviewError(ReviewErrorKind::invalid_project,
                      "project configuration is not valid JSON");
  }
}

domain::SourceKind parse_source_kind(const std::string_view value) {
  if (value == "pdf") {
    return domain::SourceKind::pdf;
  }
  if (value == "markdown") {
    return domain::SourceKind::markdown;
  }
  if (value == "text") {
    return domain::SourceKind::text;
  }
  throw ReviewError(ReviewErrorKind::state_error,
                    "project database contains an unknown source kind");
}

domain::TextStatus parse_text_status(const std::string_view value) {
  if (value == "native") {
    return domain::TextStatus::native;
  }
  if (value == "ocr_unreviewed") {
    return domain::TextStatus::ocr_unreviewed;
  }
  if (value == "reviewed") {
    return domain::TextStatus::reviewed;
  }
  if (value == "failed") {
    return domain::TextStatus::failed;
  }
  throw ReviewError(ReviewErrorKind::state_error,
                    "project database contains an unknown extraction status");
}

domain::ProposalOperation parse_operation(const std::string_view value) {
  if (value == "create_article") {
    return domain::ProposalOperation::create_article;
  }
  if (value == "update_article") {
    return domain::ProposalOperation::update_article;
  }
  throw ReviewError(ReviewErrorKind::state_error,
                    "project database contains an unknown proposal operation");
}

domain::ProposalStatus parse_proposal_status(const std::string_view value) {
  if (value == "pending") {
    return domain::ProposalStatus::pending;
  }
  if (value == "approved") {
    return domain::ProposalStatus::approved;
  }
  if (value == "rejected") {
    return domain::ProposalStatus::rejected;
  }
  if (value == "applied") {
    return domain::ProposalStatus::applied;
  }
  if (value == "superseded") {
    return domain::ProposalStatus::superseded;
  }
  throw ReviewError(ReviewErrorKind::state_error,
                    "project database contains an unknown proposal status");
}

SourceRecord find_latest_source(sqlite3* connection,
                                const domain::SourceId& source_id) {
  Statement statement(
      connection,
      "SELECT s.source_id, v.source_version_id, s.display_name, "
      "s.source_kind FROM sources s JOIN source_versions v "
      "ON v.source_id = s.source_id WHERE s.source_id = ? "
      "AND s.archived_at IS NULL ORDER BY v.rowid DESC LIMIT 1");
  statement.bind_text(1, source_id.value);
  if (statement.step() != SQLITE_ROW) {
    throw ReviewError(ReviewErrorKind::source_not_found,
                      "source does not exist, is archived, or has no version: " +
                          source_id.value);
  }
  return {.source_id = {statement.text(0)},
          .version_id = {statement.text(1)},
          .display_name = statement.text(2),
          .source_kind = parse_source_kind(statement.text(3))};
}

domain::ArticleProposal parse_payload(const std::string& payload) {
  try {
    const auto parsed =
        domain::parse_article_proposal(nlohmann::json::parse(payload));
    if (!parsed) {
      const auto detail = parsed.issues.empty()
                              ? "unknown validation error"
                              : parsed.issues.front().path + ": " +
                                    parsed.issues.front().message;
      throw ReviewError(ReviewErrorKind::state_error,
                        "stored proposal is invalid: " + detail);
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "stored proposal payload is not valid JSON");
  }
}

ProposalSummary summary_from_row(Statement& statement,
                                 const domain::ArticleProposal& proposal) {
  ProposalSummary summary{
      .proposal_id = {statement.text(0)},
      .operation = parse_operation(statement.text(2)),
      .status = parse_proposal_status(statement.text(3)),
      .title = proposal.article.title,
      .created_at = statement.text(4)};
  if (!statement.is_null(1)) {
    summary.article_id = domain::ArticleId{statement.text(1)};
  }
  if (!statement.is_null(5)) {
    summary.reviewed_at = statement.text(5);
  }
  if (!statement.is_null(6)) {
    summary.review_reason = statement.text(6);
  }
  if (summary.operation != proposal.operation) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "proposal payload operation does not match review state");
  }
  return summary;
}

bool has_visible_text(const std::string_view text) {
  return std::ranges::any_of(text, [](const char character) {
    return !std::isspace(static_cast<unsigned char>(character));
  });
}

/// Match the citation contract by trimming and collapsing ASCII whitespace.
std::string normalize_whitespace(const std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (const auto character : text) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space) {
      normalized.push_back(' ');
      pending_space = false;
    }
    normalized.push_back(character);
  }
  return normalized;
}

}  // namespace

ReviewService::ReviewService(std::filesystem::path project_root)
    : project_root_(std::filesystem::absolute(std::move(project_root))
                        .lexically_normal()),
      config_(read_config(project_root_)) {
  if (!std::filesystem::is_regular_file(project_root_ / config_.paths.state)) {
    throw ReviewError(
        ReviewErrorKind::invalid_project,
        "project state database does not exist; run 'kc init' first");
  }
}

ExtractionReview ReviewService::review_extraction(
    const domain::SourceId& source_id) const {
  try {
    storage::Database database(project_root_ / config_.paths.state);
    const auto source = find_latest_source(database.native_handle(), source_id);
    Statement statement(
        database.native_handle(),
        "SELECT page_id, page_number, image_path, text, text_status "
        "FROM source_pages WHERE source_version_id = ? ORDER BY page_number");
    statement.bind_text(1, source.version_id.value);

    ExtractionReview result{.source_id = source.source_id,
                            .source_version_id = source.version_id,
                            .display_name = source.display_name,
                            .source_kind = source.source_kind};
    while (statement.step() == SQLITE_ROW) {
      ExtractionReviewPage page{
          .page_id = {statement.text(0)},
          .page_number = static_cast<std::uint32_t>(statement.integer(1)),
          .text = statement.text(3),
          .text_status = parse_text_status(statement.text(4))};
      if (!statement.is_null(2)) {
        page.image_path = statement.text(2);
        if (!is_safe_relative(*page.image_path)) {
          throw ReviewError(
              ReviewErrorKind::state_error,
              "project database contains an unsafe rendered page path");
        }
      }
      result.pages.push_back(std::move(page));
    }
    if (result.pages.empty()) {
      throw ReviewError(
          ReviewErrorKind::invalid_state,
          "source has no extracted pages; run 'kc extract' before review");
    }
    return result;
  } catch (const ReviewError&) {
    throw;
  } catch (const storage::StorageError& error) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "could not read extraction review state: " +
                          std::string(error.what()));
  }
}

PageCorrectionResult ReviewService::correct_page_text(
    const domain::SourceId& source_id, const std::uint32_t page_number,
    std::string corrected_text) const {
  if (page_number == 0U) {
    throw ReviewError(ReviewErrorKind::invalid_input,
                      "page number must be at least 1");
  }
  if (!has_visible_text(corrected_text)) {
    throw ReviewError(ReviewErrorKind::invalid_input,
                      "corrected page text must contain visible text");
  }

  try {
    storage::Database database(project_root_ / config_.paths.state);
    Transaction transaction(database);
    const auto source = find_latest_source(database.native_handle(), source_id);
    Statement page(
        database.native_handle(),
        "SELECT page_id FROM source_pages WHERE source_version_id = ? "
        "AND page_number = ?");
    page.bind_text(1, source.version_id.value);
    page.bind_integer(2, static_cast<std::int64_t>(page_number));
    if (page.step() != SQLITE_ROW) {
      throw ReviewError(ReviewErrorKind::page_not_found,
                        "source version has no extracted page " +
                            std::to_string(page_number));
    }
    const domain::PageId page_id{page.text(0)};

    // Proposals are immutable and their normalized offsets point into this
    // exact text. A correction must therefore happen before compilation.
    Statement references(
        database.native_handle(),
        "SELECT COUNT(*) FROM proposal_citations WHERE page_id = ?");
    references.bind_text(1, page_id.value);
    static_cast<void>(references.step());
    if (references.integer(0) != 0) {
      throw ReviewError(
          ReviewErrorKind::invalid_state,
          "page text cannot be corrected after it has been cited by a proposal");
    }

    const auto hash = source_import::sha256_text(corrected_text);
    Statement update(
        database.native_handle(),
        "UPDATE source_pages SET text = ?, text_status = 'reviewed', "
        "text_sha256 = ? WHERE page_id = ? AND source_version_id = ?");
    update.bind_text(1, corrected_text);
    update.bind_text(2, hash);
    update.bind_text(3, page_id.value);
    update.bind_text(4, source.version_id.value);
    static_cast<void>(update.step());
    if (sqlite3_changes(database.native_handle()) != 1) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "page disappeared while applying correction");
    }
    transaction.commit();
    return {.source_id = source.source_id,
            .source_version_id = source.version_id,
            .page_id = page_id,
            .page_number = page_number,
            .text_sha256 = hash};
  } catch (const ReviewError&) {
    throw;
  } catch (const storage::StorageError& error) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "could not update extraction review state: " +
                          std::string(error.what()));
  }
}

std::vector<ProposalSummary> ReviewService::list_proposals(
    const std::optional<domain::ProposalStatus> status) const {
  try {
    storage::Database database(project_root_ / config_.paths.state);
    Statement statement(
        database.native_handle(),
        status ? "SELECT proposal_id, article_id, operation, status, "
                 "created_at, reviewed_at, review_reason, payload_json "
                 "FROM proposals WHERE status = ? ORDER BY created_at DESC, "
                 "proposal_id DESC"
               : "SELECT proposal_id, article_id, operation, status, "
                 "created_at, reviewed_at, review_reason, payload_json "
                 "FROM proposals ORDER BY created_at DESC, proposal_id DESC");
    if (status) {
      statement.bind_text(1, domain::to_string(*status));
    }

    std::vector<ProposalSummary> result;
    while (statement.step() == SQLITE_ROW) {
      const auto proposal = parse_payload(statement.text(7));
      result.push_back(summary_from_row(statement, proposal));
    }
    return result;
  } catch (const ReviewError&) {
    throw;
  } catch (const storage::StorageError& error) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "could not list proposal review state: " +
                          std::string(error.what()));
  }
}

ProposalReview ReviewService::review_proposal(
    const domain::ProposalId& proposal_id) const {
  try {
    storage::Database database(project_root_ / config_.paths.state);
    Statement proposal_row(
        database.native_handle(),
        "SELECT proposal_id, article_id, operation, status, created_at, "
        "reviewed_at, review_reason, payload_json FROM proposals "
        "WHERE proposal_id = ?");
    proposal_row.bind_text(1, proposal_id.value);
    if (proposal_row.step() != SQLITE_ROW) {
      throw ReviewError(ReviewErrorKind::proposal_not_found,
                        "proposal does not exist: " + proposal_id.value);
    }
    auto proposal = parse_payload(proposal_row.text(7));
    ProposalReview result{
        .summary = summary_from_row(proposal_row, proposal),
        .proposal = std::move(proposal)};

    Statement citations(
        database.native_handle(),
        "SELECT pc.section_key, pc.block_index, pc.page_id, pc.start_char, "
        "pc.end_char, pc.quote, s.source_id, s.display_name, s.source_kind, "
        "v.source_version_id, p.page_number, p.image_path, p.text, "
        "p.text_status FROM proposal_citations pc "
        "JOIN source_pages p ON p.page_id = pc.page_id "
        "JOIN source_versions v ON v.source_version_id = p.source_version_id "
        "JOIN sources s ON s.source_id = v.source_id "
        "WHERE pc.proposal_id = ? ORDER BY pc.section_key, pc.block_index, "
        "pc.page_id, pc.start_char");
    citations.bind_text(1, proposal_id.value);
    while (citations.step() == SQLITE_ROW) {
      CitationEvidence evidence{
          .section_key = citations.text(0),
          .block_index = static_cast<std::size_t>(citations.integer(1)),
          .citation = {.page_id = {citations.text(2)},
                       .start_char =
                           static_cast<std::size_t>(citations.integer(3)),
                       .end_char =
                           static_cast<std::size_t>(citations.integer(4)),
                       .quote = citations.text(5)},
          .source_id = {citations.text(6)},
          .source_name = citations.text(7),
          .source_kind = parse_source_kind(citations.text(8)),
          .source_version_id = {citations.text(9)},
          .page_number =
              static_cast<std::uint32_t>(citations.integer(10)),
          .extracted_text = citations.text(12),
          .text_status = parse_text_status(citations.text(13))};
      if (!citations.is_null(11)) {
        evidence.image_path = citations.text(11);
        if (!is_safe_relative(*evidence.image_path)) {
          throw ReviewError(
              ReviewErrorKind::state_error,
              "proposal citation contains an unsafe rendered page path");
        }
      }
      if (evidence.citation.start_char >= evidence.citation.end_char ||
          evidence.citation.end_char > evidence.extracted_text.size()) {
        throw ReviewError(ReviewErrorKind::state_error,
                          "proposal citation offsets no longer fit page text");
      }
      const auto cited_text = evidence.extracted_text.substr(
          evidence.citation.start_char,
          evidence.citation.end_char - evidence.citation.start_char);
      if (normalize_whitespace(cited_text) != evidence.citation.quote) {
        throw ReviewError(
            ReviewErrorKind::state_error,
            "proposal citation quote no longer matches extracted page text");
      }
      result.citation_evidence.push_back(std::move(evidence));
    }
    if (result.citation_evidence.empty()) {
      throw ReviewError(ReviewErrorKind::state_error,
                        "proposal has no normalized citation evidence");
    }
    return result;
  } catch (const ReviewError&) {
    throw;
  } catch (const storage::StorageError& error) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "could not read proposal review state: " +
                          std::string(error.what()));
  }
}

void ReviewService::reject_proposal(
    const domain::ProposalId& proposal_id,
    std::optional<std::string> reason) const {
  if (reason && reason->size() > 4096U) {
    throw ReviewError(ReviewErrorKind::invalid_input,
                      "rejection reason must be at most 4096 bytes");
  }
  if (reason && !has_visible_text(*reason)) {
    throw ReviewError(ReviewErrorKind::invalid_input,
                      "rejection reason must contain visible text");
  }

  try {
    storage::Database database(project_root_ / config_.paths.state);
    Statement exists(database.native_handle(),
                     "SELECT status FROM proposals WHERE proposal_id = ?");
    exists.bind_text(1, proposal_id.value);
    if (exists.step() != SQLITE_ROW) {
      throw ReviewError(ReviewErrorKind::proposal_not_found,
                        "proposal does not exist: " + proposal_id.value);
    }
    if (exists.text(0) != "pending") {
      throw ReviewError(ReviewErrorKind::invalid_state,
                        "only a pending proposal can be rejected");
    }

    Statement update(
        database.native_handle(),
        "UPDATE proposals SET status = 'rejected', reviewed_at = ?, "
        "review_reason = ? WHERE proposal_id = ? AND status = 'pending'");
    update.bind_text(1, utc_now());
    if (reason) {
      update.bind_text(2, *reason);
    } else {
      update.bind_null(2);
    }
    update.bind_text(3, proposal_id.value);
    static_cast<void>(update.step());
    if (sqlite3_changes(database.native_handle()) != 1) {
      throw ReviewError(ReviewErrorKind::invalid_state,
                        "proposal is no longer pending");
    }
  } catch (const ReviewError&) {
    throw;
  } catch (const storage::StorageError& error) {
    throw ReviewError(ReviewErrorKind::state_error,
                      "could not reject proposal: " +
                          std::string(error.what()));
  }
}

}  // namespace kc::review
