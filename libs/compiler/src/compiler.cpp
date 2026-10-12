#include "kc/compiler/compiler.hpp"

#include "kc/import/integrity.hpp"
#include "kc/domain/path.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/concept.hpp"
#include "kc/domain/json.hpp"
#include "kc/import/sha256.hpp"
#include "kc/models/model_runner.hpp"
#include "kc/models/proposal_validator.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/concept_identity.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kc::compiler {
namespace {

/// Small RAII wrapper used for the compiler's parameterized SQLite queries.
class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_,
                           nullptr) != SQLITE_OK) {
      throw CompilerError(
          CompilerErrorKind::state_error,
          "failed to prepare compiler state query: " +
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
      throw CompilerError(
          CompilerErrorKind::state_error,
          "failed to bind compiler state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw CompilerError(
          CompilerErrorKind::state_error,
          "failed to bind compiler state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_null(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw CompilerError(
          CompilerErrorKind::state_error,
          "failed to bind compiler state query: " +
              std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw CompilerError(
          CompilerErrorKind::state_error,
          "compiler state query failed: " +
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

/// Roll back an unfinished transaction on every exception path.
class Transaction {
 public:
  explicit Transaction(storage::Database& database) : database_(database) {
    database_.execute("BEGIN IMMEDIATE;");
  }

  ~Transaction() {
    if (!committed_) {
      try {
        database_.execute("ROLLBACK;");
      } catch (const storage::StorageError&) {
        // Preserve the original exception; rollback is best-effort here.
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

/// Persist and compare all generation inputs, including uncited context.
/// Revision prevents an A -> B -> A edit from appearing unchanged.
nlohmann::json evidence_snapshot(const Statement& row) {
  return {{"page_id", row.text(0)}, {"version_id", row.text(1)},
          {"page_number", row.integer(2)}, {"image_path", row.text(3)},
          {"text", row.text(4)}, {"status", row.text(5)},
          {"source_id", row.text(6)}, {"source_name", row.text(7)},
          {"revision", row.integer(8)}, {"text_sha256", row.text(9)},
          {"image_sha256", row.text(10)}, {"stored_path", row.text(11)},
          {"source_sha256", row.text(12)}};
}

struct CandidatePage {
  domain::ExtractedPage page;
  std::string source_id;
  std::string source_name;
  std::string normalized_source_name;
  std::size_t relevance_score{0};
  nlohmann::json evidence;
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

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  if (!domain::is_within(root, root / "kc.json")) {
    throw CompilerError(CompilerErrorKind::invalid_project, "configuration escapes project root");
  }
  std::ifstream input(root / "kc.json");
  if (!input) {
    throw CompilerError(CompilerErrorKind::invalid_project,
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
      throw CompilerError(CompilerErrorKind::invalid_project,
                          "invalid project configuration: " + detail);
    }
    try {
      domain::validate_project_paths(root, *parsed.value);
    } catch (const std::invalid_argument& error) {
      throw CompilerError(CompilerErrorKind::invalid_project, error.what());
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    throw CompilerError(CompilerErrorKind::invalid_project,
                        "project configuration is not valid JSON");
  }
}

domain::TextStatus parse_text_status(const std::string_view status) {
  if (status == "native") {
    return domain::TextStatus::native;
  }
  if (status == "ocr_unreviewed") {
    return domain::TextStatus::ocr_unreviewed;
  }
  if (status == "reviewed") {
    return domain::TextStatus::reviewed;
  }
  if (status == "failed") {
    return domain::TextStatus::failed;
  }
  throw CompilerError(CompilerErrorKind::state_error,
                      "project database contains an unknown page text status");
}

/// Make title and phrase comparison stable across capitalization and common
/// punctuation without changing the original evidence text or byte offsets.
std::string normalize_match_text(const std::string_view text) {
  try {
    return domain::normalize_lexical_text(text);
  } catch (const std::invalid_argument&) {
    // Malformed text cannot establish lexical relevance. Explicit curation
    // remains separate from automatic matching and extraction validation.
    return {};
  }
}

bool contains_phrase(const std::string& normalized_text,
                     const std::string_view phrase) {
  if (phrase.empty() || normalized_text.empty()) {
    return false;
  }
  const auto padded_text = " " + normalized_text + " ";
  const auto padded_phrase = " " + std::string(phrase) + " ";
  return padded_text.find(padded_phrase) != std::string::npos;
}

std::vector<std::string> concept_terms(const std::string& normalized_title) {
  // Function words do not distinguish topics, but remain part of the higher
  // confidence full-title phrase match above this fallback.
  static const std::unordered_set<std::string> stop_words{
      "a", "an", "and", "as", "at", "by", "for", "from", "in",
      "into", "of", "on", "or", "the", "to", "with"};
  std::istringstream input(normalized_title);
  std::vector<std::string> all_terms;
  std::vector<std::string> meaningful_terms;
  for (std::string term; input >> term;) {
    all_terms.push_back(term);
    if (!stop_words.contains(term)) {
      meaningful_terms.push_back(std::move(term));
    }
  }
  return meaningful_terms.empty() ? all_terms : meaningful_terms;
}

bool contains_term_variant(const std::string& normalized_text,
                           const std::string& term) {
  if (contains_phrase(normalized_text, term)) {
    return true;
  }
  if (!std::ranges::all_of(term, [](const char character) {
        return character >= 'a' && character <= 'z';
      })) {
    return false;
  }
  // A small deterministic singular/plural fallback covers common concept
  // title differences without embedding domain-specific aliases.
  if (term.size() > 3U && term.ends_with('s')) {
    return contains_phrase(normalized_text,
                           std::string_view(term).substr(0, term.size() - 1U));
  }
  return contains_phrase(normalized_text, term + "s");
}

bool contains_all_terms(const std::string& normalized_text,
                        const std::vector<std::string>& terms) {
  return !terms.empty() &&
         std::ranges::all_of(terms, [&](const std::string& term) {
           return contains_term_variant(normalized_text, term);
         });
}

/// Full concept-title matches dominate all-term matches. The score is used
/// only for deterministic ordering; it contains no subject-specific keywords.
std::size_t relevance_score(const std::string& source_name,
                            const std::string& page_text,
                            const std::string& concept_title) {
  const auto normalized_name = normalize_match_text(source_name);
  const auto normalized_text = normalize_match_text(page_text);
  const auto normalized_title = normalize_match_text(concept_title);
  const auto terms = concept_terms(normalized_title);
  std::size_t score = 0;
  if (contains_phrase(normalized_name, normalized_title)) {
    score += 1'000;
  }
  if (contains_phrase(normalized_text, normalized_title)) {
    score += 800;
  }
  if (contains_all_terms(normalized_name, terms)) {
    score += 300;
  }
  if (contains_all_terms(normalized_text, terms)) {
    score += 200;
  }
  return score;
}

/// Citation comparison permits whitespace variations; persisted quotes use a
/// single-space canonical representation derived from the cited page bytes.
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

void normalize_citation(
    domain::Citation& citation,
    const std::unordered_map<std::string, const domain::ExtractedPage*>& pages) {
  const auto found = pages.find(citation.page_id.value);
  if (found == pages.end() || citation.start_char >= citation.end_char ||
      citation.end_char > found->second->text.size()) {
    return;
  }
  citation.quote = normalize_whitespace(found->second->text.substr(
      citation.start_char, citation.end_char - citation.start_char));
}

void normalize_proposal_citations(
    domain::ArticleProposal& proposal,
    const std::vector<domain::ExtractedPage>& pages) {
  std::unordered_map<std::string, const domain::ExtractedPage*> page_index;
  for (const auto& page : pages) {
    page_index.emplace(page.page_id.value, &page);
  }
  for (auto& section : proposal.sections) {
    for (auto& block : section.blocks) {
      for (auto& citation : block.citations) {
        normalize_citation(citation, page_index);
      }
    }
  }
  for (auto& related : proposal.related_concepts) {
    for (auto& citation : related.citations) {
      normalize_citation(citation, page_index);
    }
  }
}

std::string first_issue(const std::vector<domain::ValidationIssue>& issues,
                        const std::string_view fallback) {
  if (issues.empty()) {
    return std::string(fallback);
  }
  return issues.front().path + ": " + issues.front().message;
}

CompilerErrorKind compiler_kind(const models::ModelErrorKind kind) {
  switch (kind) {
    case models::ModelErrorKind::invalid_project:
      return CompilerErrorKind::invalid_project;
    case models::ModelErrorKind::invalid_request:
      return CompilerErrorKind::validation_failed;
    case models::ModelErrorKind::state_error:
      return CompilerErrorKind::state_error;
  }
  return CompilerErrorKind::state_error;
}

std::unique_ptr<models::ModelRunner> make_model_runner(
    const std::filesystem::path& project_root) {
  try {
    return std::make_unique<models::ModelRunner>(project_root);
  } catch (const models::ModelError& error) {
    throw CompilerError(compiler_kind(error.kind()), error.what());
  }
}

std::unique_ptr<models::ModelRunner> make_model_runner(
    const std::filesystem::path& project_root,
    std::unique_ptr<models::LanguageModel> language_model) {
  try {
    return std::make_unique<models::ModelRunner>(
        project_root, std::move(language_model));
  } catch (const models::ModelError& error) {
    throw CompilerError(compiler_kind(error.kind()), error.what());
  }
}

}  // namespace

class Compiler::Impl {
 public:
  explicit Impl(std::filesystem::path project_root)
      : project_root_(std::filesystem::absolute(std::move(project_root))
                          .lexically_normal()),
        config_(read_config(project_root_)),
        model_runner_(make_model_runner(project_root_)) {
    verify_state_database();
  }

  Impl(std::filesystem::path project_root,
       std::unique_ptr<models::LanguageModel> language_model)
      : project_root_(std::filesystem::absolute(std::move(project_root))
                          .lexically_normal()),
        config_(read_config(project_root_)),
        model_runner_(make_model_runner(project_root_,
                                        std::move(language_model))) {
    verify_state_database();
  }

  [[nodiscard]] std::vector<domain::ExtractedPage> select_evidence(
      const CompileOptions& options,
      std::unordered_map<std::string, nlohmann::json>* snapshots = nullptr) const {
    validate_options(options);
    static_cast<void>(read_config(project_root_));

    try {
      storage::Database database(state_path());
      const auto requested_sources = validate_source_filters(
          database.native_handle(), options.source_ids);
      Statement statement(
          database.native_handle(),
          "SELECT p.page_id, p.source_version_id, p.page_number, "
          "p.image_path, p.text, p.text_status, s.source_id, s.display_name, "
          "p.evidence_revision, p.text_sha256, p.image_sha256, v.stored_path, v.sha256 "
          "FROM source_pages p "
          "JOIN source_versions v "
          "ON v.source_version_id = p.source_version_id "
          "JOIN sources s ON s.source_id = v.source_id "
          "WHERE s.archived_at IS NULL "
          "AND v.rowid = (SELECT MAX(v2.rowid) FROM source_versions v2 "
          "WHERE v2.source_id = s.source_id) "
          "ORDER BY lower(s.display_name), s.source_id, p.page_number, "
          "p.page_id");

      std::vector<CandidatePage> candidates;
      while (statement.step() == SQLITE_ROW) {
        const auto source_id = statement.text(6);
        if (!requested_sources.empty() &&
            !requested_sources.contains(source_id)) {
          continue;
        }
        const auto status = parse_text_status(statement.text(5));
        const auto text = statement.text(4);
        // Native extraction is usable immediately, while OCR must cross the
        // explicit human-review boundary before any text reaches the model.
        const auto model_ready = status == domain::TextStatus::native ||
                                 status == domain::TextStatus::reviewed;
        if (!model_ready || text.empty()) {
          continue;
        }

        CandidatePage candidate{
            .page = {.page_id = {statement.text(0)},
                     .source_version_id = {statement.text(1)},
                     .page_number =
                         static_cast<std::uint32_t>(statement.integer(2)),
                     .text = text,
                     .text_status = status},
            .source_id = source_id,
            .source_name = statement.text(7)};
        if (!statement.is_null(3)) {
          candidate.page.image_path = domain::path_from_utf8(statement.text(3));
        }
        candidate.evidence = evidence_snapshot(statement);
        candidate.normalized_source_name =
            normalize_match_text(candidate.source_name);
        candidate.relevance_score =
            relevance_score(candidate.source_name, candidate.page.text,
                            options.concept_title);
        if (candidate.relevance_score != 0U || !requested_sources.empty()) {
          candidates.push_back(std::move(candidate));
        }
      }

      // Explicit source filters are a human-curated escape hatch for topics
      // that are described implicitly, symbolically, or in another language.
      // If lexical matching found anything, keep its normal precision.
      const auto has_lexical_match =
          std::ranges::any_of(candidates, [](const CandidatePage& candidate) {
            return candidate.relevance_score != 0U;
          });
      if (has_lexical_match) {
        std::erase_if(candidates, [](const CandidatePage& candidate) {
          return candidate.relevance_score == 0U;
        });
      }

      std::ranges::sort(candidates, [](const CandidatePage& left,
                                      const CandidatePage& right) {
        if (left.relevance_score != right.relevance_score) {
          return left.relevance_score > right.relevance_score;
        }
        if (left.normalized_source_name != right.normalized_source_name) {
          return left.normalized_source_name < right.normalized_source_name;
        }
        if (left.source_id != right.source_id) {
          return left.source_id < right.source_id;
        }
        if (left.page.page_number != right.page.page_number) {
          return left.page.page_number < right.page.page_number;
        }
        return left.page.page_id.value < right.page.page_id.value;
      });

      std::vector<domain::ExtractedPage> pages;
      pages.reserve(candidates.size());
      for (auto& candidate : candidates) {
        try {
          if (!domain::is_valid_source_text(candidate.page.text) ||
              source_import::sha256_text(candidate.page.text) !=
                  candidate.evidence["text_sha256"].get<std::string>()) {
            throw std::invalid_argument("selected page has invalid encoding or text SHA-256");
          }
          source_import::verify_retained_file(project_root_ / config_.paths.sources,
              project_root_ / domain::path_from_utf8(candidate.evidence["stored_path"].get<std::string>()),
              candidate.evidence["source_sha256"].get<std::string>());
          if (candidate.page.image_path) {
            source_import::verify_retained_file(project_root_ / config_.paths.cache,
                project_root_ / *candidate.page.image_path,
                candidate.evidence["image_sha256"].get<std::string>());
          }
        } catch (const std::exception& error) {
          throw CompilerError(CompilerErrorKind::validation_failed, error.what());
        }
        if (snapshots) {
          snapshots->emplace(candidate.page.page_id.value, candidate.evidence);
        }
        pages.push_back(std::move(candidate.page));
      }
      return pages;
    } catch (const storage::StorageError& error) {
      throw CompilerError(CompilerErrorKind::state_error,
                          "could not read compiler state: " +
                              std::string(error.what()));
    }
  }

  [[nodiscard]] CompileResult compile(const CompileOptions& options) {
    validate_options(options);
    const auto target = find_article_target(options.concept_title);
    auto canonical_options = options;
    canonical_options.concept_title = target.title;
    std::unordered_map<std::string, nlohmann::json> snapshots;
    auto pages = select_evidence(canonical_options, &snapshots);
    if (pages.empty()) {
      throw CompilerError(
          CompilerErrorKind::no_evidence,
          "no usable extracted pages matched concept '" +
              options.concept_title +
              "'; pass --source to compile from explicitly curated sources");
    }

    const auto operation = target.article_id
        ? domain::ProposalOperation::update_article
        : domain::ProposalOperation::create_article;
    models::ProposalGenerationRequest request{
        .concept_title = target.title,
        .operation = operation,
        .article_id = target.article_id,
        .concept_slug = target.slug,
        .pages = std::move(pages)};
    models::ModelRunResult model_result;
    try {
      model_result = model_runner_->generate_concept(request);
    } catch (const models::ModelError& error) {
      throw CompilerError(compiler_kind(error.kind()), error.what());
    }
    if (model_result.status == domain::ModelRunStatus::failed) {
      throw CompilerError(
          CompilerErrorKind::model_failed,
          first_issue(model_result.issues, "local model request failed"));
    }
    if (model_result.status == domain::ModelRunStatus::invalid_response ||
        !model_result.proposal) {
      throw CompilerError(
          CompilerErrorKind::validation_failed,
          first_issue(model_result.issues,
                      "model response failed proposal validation"));
    }

    auto proposal = *model_result.proposal;
    normalize_proposal_citations(proposal, request.pages);
    const auto validation = validator_.validate_response(proposal, request);
    if (!validation) {
      mark_model_run_invalid(model_result.run_id, proposal,
                             validation.issues.size());
      throw CompilerError(
          CompilerErrorKind::validation_failed,
          first_issue(validation.issues,
                      "proposal failed compiler validation"));
    }

    const auto proposal_id = domain::generate_proposal_id();
    persist_proposal(proposal_id, model_result.run_id, proposal,
                     target.article_id, snapshots);
    return {
        .proposal_id = proposal_id,
        .model_run_id = model_result.run_id,
        .operation = operation,
        .article_id = target.article_id,
        .selected_page_count = request.pages.size()};
  }

 private:
  [[nodiscard]] std::filesystem::path state_path() const {
    try {
      domain::validate_project_paths(project_root_, config_);
    } catch (const std::invalid_argument& error) {
      throw CompilerError(CompilerErrorKind::invalid_project, error.what());
    }
    return project_root_ / config_.paths.state;
  }

  void verify_state_database() const {
    if (!std::filesystem::is_regular_file(state_path())) {
      throw CompilerError(
          CompilerErrorKind::invalid_project,
          "project state database does not exist; run 'kc init' first");
    }
  }

  static void validate_options(const CompileOptions& options) {
    try {
      static_cast<void>(domain::normalize_concept_title(options.concept_title));
    } catch (const std::invalid_argument& error) {
      throw CompilerError(CompilerErrorKind::invalid_concept, error.what());
    }
    for (const auto& source_id : options.source_ids) {
      if (source_id.value.empty()) {
        throw CompilerError(
            CompilerErrorKind::source_not_found,
            "source filters must not be empty");
      }
    }
  }

  static std::unordered_set<std::string> validate_source_filters(
      sqlite3* connection,
      const std::vector<domain::SourceId>& source_ids) {
    if (source_ids.empty()) {
      return {};
    }

    std::unordered_set<std::string> requested;
    for (const auto& source_id : source_ids) {
      requested.insert(source_id.value);
    }
    Statement statement(
        connection,
        "SELECT source_id FROM sources WHERE archived_at IS NULL");
    while (statement.step() == SQLITE_ROW) {
      requested.erase(statement.text(0));
    }
    if (!requested.empty()) {
      throw CompilerError(
          CompilerErrorKind::source_not_found,
          "requested source does not exist or is archived: " +
              *std::ranges::min_element(requested));
    }

    std::unordered_set<std::string> result;
    for (const auto& source_id : source_ids) {
      result.insert(source_id.value);
    }
    return result;
  }

  [[nodiscard]] storage::ConceptTarget find_article_target(
      const std::string_view concept_title) const {
    try {
      storage::Database database(state_path());
      auto target = storage::resolve_concept(database.native_handle(), concept_title);
      const auto path = target.vault_path.value_or(
          config_.paths.vault / domain::concept_filename(target.title));
      storage::validate_concept_destination(database.native_handle(), path, target.article_id);
      return target;
    } catch (const storage::ConceptIdentityError& error) {
      throw CompilerError(CompilerErrorKind::invalid_concept, error.what());
    } catch (const storage::StorageError& error) {
      throw CompilerError(CompilerErrorKind::state_error, error.what());
    }
  }

  void mark_model_run_invalid(const domain::RunId& run_id,
                              const domain::ArticleProposal& proposal,
                              const std::size_t issue_count) const {
    const auto serialized = nlohmann::json(proposal).dump();
    const auto diagnostic = nlohmann::json{
        {"redacted", true},
        {"response_sha256", source_import::sha256_text(serialized)},
        {"response_bytes", serialized.size()},
        {"validation_issue_count", issue_count}}
                                .dump();
    try {
      storage::Database database(state_path());
      Statement statement(
          database.native_handle(),
          "UPDATE model_runs SET status = 'invalid_response', "
          "response_json = ?, error_message = ? WHERE run_id = ?");
      statement.bind_text(1, diagnostic);
      statement.bind_text(2, "model response failed compiler validation");
      statement.bind_text(3, run_id.value);
      static_cast<void>(statement.step());
    } catch (const storage::StorageError& error) {
      throw CompilerError(CompilerErrorKind::state_error,
                          "could not update model-run validation state: " +
                              std::string(error.what()));
    }
  }

  static void insert_citation(sqlite3* connection,
                              const domain::ProposalId& proposal_id,
                              const std::string_view section_key,
                              const std::size_t block_index,
                              const domain::Citation& citation) {
    Statement statement(
        connection,
        "INSERT OR IGNORE INTO proposal_citations("
        "proposal_id, section_key, block_index, page_id, start_char, "
        "end_char, quote) VALUES (?, ?, ?, ?, ?, ?, ?)");
    statement.bind_text(1, proposal_id.value);
    statement.bind_text(2, section_key);
    statement.bind_integer(3, static_cast<std::int64_t>(block_index));
    statement.bind_text(4, citation.page_id.value);
    statement.bind_integer(5,
                           static_cast<std::int64_t>(citation.start_char));
    statement.bind_integer(6,
                           static_cast<std::int64_t>(citation.end_char));
    statement.bind_text(7, citation.quote);
    static_cast<void>(statement.step());
  }

  void persist_proposal(
      const domain::ProposalId& proposal_id,
      const domain::RunId& model_run_id,
      const domain::ArticleProposal& proposal,
      const std::optional<domain::ArticleId>& article_id,
      const std::unordered_map<std::string, nlohmann::json>& snapshots) const {
    static_cast<void>(read_config(project_root_));
    try {
      storage::Database database(state_path());
      Transaction transaction(database);

      // Recheck every selected page under the publication write lock. A stale
      // generation cannot supersede prior proposals or publish citations.
      for (const auto& [page_id, expected] : snapshots) {
        Statement page(database.native_handle(),
            "SELECT p.page_id, p.source_version_id, p.page_number, p.image_path, "
            "p.text, p.text_status, s.source_id, s.display_name, p.evidence_revision, "
            "p.text_sha256, p.image_sha256, v.stored_path, v.sha256 "
            "FROM source_pages p JOIN source_versions v ON v.source_version_id = p.source_version_id "
            "JOIN sources s ON s.source_id = v.source_id WHERE p.page_id = ? "
            "AND s.archived_at IS NULL AND v.rowid = "
            "(SELECT MAX(v2.rowid) FROM source_versions v2 WHERE v2.source_id = s.source_id)");
        page.bind_text(1, page_id);
        if (page.step() != SQLITE_ROW || evidence_snapshot(page) != expected) {
          throw CompilerError(CompilerErrorKind::validation_failed,
                              "selected evidence changed during generation; review and compile again");
        }
        try {
          source_import::verify_retained_file(project_root_ / config_.paths.sources,
              project_root_ / domain::path_from_utf8(page.text(11)), page.text(12));
          if (!page.is_null(3)) {
            source_import::verify_retained_file(project_root_ / config_.paths.cache,
                project_root_ / domain::path_from_utf8(page.text(3)), page.text(10));
          }
        } catch (const std::exception& error) {
          throw CompilerError(CompilerErrorKind::validation_failed, error.what());
        }
      }

      // Recheck after generation under the write lock: another compiler or
      // apply may have claimed the identity while the model was running.
      const auto current = storage::resolve_concept(
          database.native_handle(), proposal.article.title);
      storage::validate_concept_destination(database.native_handle(),
          current.vault_path.value_or(config_.paths.vault /
                                     domain::concept_filename(current.title)), article_id);
      if (current.article_id != article_id ||
          current.title != proposal.article.title ||
          current.slug != proposal.article.slug) {
        throw CompilerError(CompilerErrorKind::invalid_concept,
                            "concept identity changed during generation; compile again");
      }

      if (article_id) {
        Statement supersede(database.native_handle(),
            "UPDATE proposals SET status = 'superseded' WHERE article_id = ? "
            "AND status IN ('pending', 'approved')");
        supersede.bind_text(1, article_id->value);
        static_cast<void>(supersede.step());
      }
      // Use the same Unicode key for pending creates, including legacy
      // capitalization and whitespace variants; unrelated concepts survive.
      for (const auto& pending : current.pending_creates) {
        Statement supersede(database.native_handle(),
            "UPDATE proposals SET status = 'superseded' WHERE proposal_id = ? "
            "AND status IN ('pending', 'approved')");
        supersede.bind_text(1, pending.value);
        static_cast<void>(supersede.step());
      }

      const nlohmann::json payload = proposal;
      Statement insert(
          database.native_handle(),
          "INSERT INTO proposals(proposal_id, article_id, model_run_id, "
          "operation, payload_json, status, created_at) "
          "VALUES (?, ?, ?, ?, ?, 'pending', ?)");
      insert.bind_text(1, proposal_id.value);
      if (article_id) {
        insert.bind_text(2, article_id->value);
      } else {
        insert.bind_null(2);
      }
      insert.bind_text(3, model_run_id.value);
      insert.bind_text(4, domain::to_string(proposal.operation));
      insert.bind_text(5, payload.dump());
      insert.bind_text(6, utc_now());
      static_cast<void>(insert.step());

      for (const auto& [page_id, evidence] : snapshots) {
        Statement insert_evidence(database.native_handle(),
            "INSERT INTO proposal_evidence(proposal_id, page_id, evidence_revision, "
            "text_sha256, image_sha256, source_sha256) VALUES (?, ?, ?, ?, ?, ?)");
        insert_evidence.bind_text(1, proposal_id.value);
        insert_evidence.bind_text(2, page_id);
        insert_evidence.bind_integer(3, evidence["revision"].get<std::int64_t>());
        insert_evidence.bind_text(4, evidence["text_sha256"].get<std::string>());
        if (evidence["image_sha256"].get<std::string>().empty()) {
          insert_evidence.bind_null(5);
        } else {
          insert_evidence.bind_text(5, evidence["image_sha256"].get<std::string>());
        }
        insert_evidence.bind_text(6, evidence["source_sha256"].get<std::string>());
        static_cast<void>(insert_evidence.step());
      }

      // Block indexes are contiguous per section key, even if a model emits
      // more than one section with the same key. Top-level related concepts
      // continue the related_concepts index so no normalized citation is lost
      // to the table's composite primary key.
      std::unordered_map<std::string, std::size_t> next_block_index;
      for (const auto& section : proposal.sections) {
        const auto section_key = std::string(domain::to_string(section.key));
        for (const auto& block : section.blocks) {
          const auto block_index = next_block_index[section_key]++;
          for (const auto& citation : block.citations) {
            insert_citation(database.native_handle(), proposal_id,
                            section_key, block_index, citation);
          }
        }
      }
      for (const auto& related : proposal.related_concepts) {
        const auto block_index = next_block_index["related_concepts"]++;
        for (const auto& citation : related.citations) {
          insert_citation(database.native_handle(), proposal_id,
                          "related_concepts", block_index, citation);
        }
      }
      transaction.commit();
    } catch (const storage::ConceptIdentityError& error) {
      throw CompilerError(CompilerErrorKind::invalid_concept, error.what());
    } catch (const storage::StorageError& error) {
      throw CompilerError(CompilerErrorKind::state_error,
                          "could not persist proposal: " +
                              std::string(error.what()));
    }
  }

  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
  std::unique_ptr<models::ModelRunner> model_runner_;
  models::ProposalValidator validator_;
};

Compiler::Compiler(std::filesystem::path project_root)
    : impl_(std::make_unique<Impl>(std::move(project_root))) {}

Compiler::Compiler(std::filesystem::path project_root,
                   std::unique_ptr<models::LanguageModel> language_model)
    : impl_(std::make_unique<Impl>(std::move(project_root),
                                   std::move(language_model))) {}

Compiler::~Compiler() = default;
Compiler::Compiler(Compiler&&) noexcept = default;
Compiler& Compiler::operator=(Compiler&&) noexcept = default;

std::vector<domain::ExtractedPage> Compiler::select_evidence(
    const CompileOptions& options) const {
  return impl_->select_evidence(options);
}

CompileResult Compiler::compile(const CompileOptions& options) {
  return impl_->compile(options);
}

}  // namespace kc::compiler
