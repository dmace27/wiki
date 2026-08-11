#include "kc/compiler/compiler.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/json.hpp"
#include "kc/import/sha256.hpp"
#include "kc/models/model_runner.hpp"
#include "kc/models/proposal_validator.hpp"
#include "kc/storage/database.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
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

struct CandidatePage {
  domain::ExtractedPage page;
  std::string source_id;
  std::string source_name;
  std::string normalized_source_name;
  std::size_t relevance_score{0};
};

struct ArticleTarget {
  domain::ProposalOperation operation{
      domain::ProposalOperation::create_article};
  std::optional<domain::ArticleId> article_id;
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
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (const auto character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte)) {
      if (pending_space && !normalized.empty()) {
        normalized.push_back(' ');
      }
      normalized.push_back(static_cast<char>(std::tolower(byte)));
      pending_space = false;
    } else {
      pending_space = !normalized.empty();
    }
  }
  return normalized;
}

bool contains_phrase(const std::string& normalized_text,
                     const std::string_view phrase) {
  const auto padded_text = " " + normalized_text + " ";
  const auto padded_phrase = " " + std::string(phrase) + " ";
  return padded_text.find(padded_phrase) != std::string::npos;
}

/// Title and alias matches dominate topic-specific keyword matches. The score
/// is used only for deterministic ordering; every positive match is retained.
std::size_t relevance_score(const std::string& source_name,
                            const std::string& page_text) {
  constexpr std::string_view title = "markov chains";
  constexpr std::string_view alias = "markov chain";
  constexpr std::array<std::string_view, 8> keywords{
      "markov property",       "transition probability",
      "transition probabilities", "transition matrix",
      "state transition",     "stochastic matrix",
      "memoryless",           "memorylessness"};

  const auto normalized_name = normalize_match_text(source_name);
  const auto normalized_text = normalize_match_text(page_text);
  std::size_t score = 0;
  if (contains_phrase(normalized_name, title)) {
    score += 1'000;
  }
  if (contains_phrase(normalized_text, title)) {
    score += 800;
  }
  if (contains_phrase(normalized_name, alias)) {
    score += 600;
  }
  if (contains_phrase(normalized_text, alias)) {
    score += 400;
  }
  for (const auto keyword : keywords) {
    if (contains_phrase(normalized_name, keyword)) {
      score += 40;
    }
    if (contains_phrase(normalized_text, keyword)) {
      score += 20;
    }
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
      const CompileOptions& options) const {
    validate_options(options);

    try {
      storage::Database database(state_path());
      const auto requested_sources = validate_source_filters(
          database.native_handle(), options.source_ids);
      Statement statement(
          database.native_handle(),
          "SELECT p.page_id, p.source_version_id, p.page_number, "
          "p.image_path, p.text, p.text_status, s.source_id, s.display_name "
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
        if (status == domain::TextStatus::failed || text.empty()) {
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
          candidate.page.image_path = statement.text(3);
        }
        candidate.normalized_source_name =
            normalize_match_text(candidate.source_name);
        candidate.relevance_score =
            relevance_score(candidate.source_name, candidate.page.text);
        if (candidate.relevance_score != 0U) {
          candidates.push_back(std::move(candidate));
        }
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
    auto pages = select_evidence(options);
    if (pages.empty()) {
      throw CompilerError(
          CompilerErrorKind::no_evidence,
          "no extracted pages matched the Markov Chains title, alias, or "
          "keywords");
    }

    const auto target = find_article_target();
    models::ProposalGenerationRequest request{
        .concept_title = options.concept_title,
        .operation = target.operation,
        .article_id = target.article_id,
        .pages = std::move(pages)};
    models::ModelRunResult model_result;
    try {
      model_result = model_runner_->generate_markov_chains(request);
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
                     target.article_id);
    return {
        .proposal_id = proposal_id,
        .model_run_id = model_result.run_id,
        .operation = target.operation,
        .article_id = target.article_id,
        .selected_page_count = request.pages.size()};
  }

 private:
  [[nodiscard]] std::filesystem::path state_path() const {
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
    if (options.concept_title != "Markov Chains") {
      throw CompilerError(
          CompilerErrorKind::unsupported_concept,
          "MVP compilation supports exactly 'Markov Chains'");
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

  [[nodiscard]] ArticleTarget find_article_target() const {
    try {
      storage::Database database(state_path());
      Statement statement(
          database.native_handle(),
          "SELECT article_id FROM articles WHERE title = ? COLLATE NOCASE "
          "LIMIT 1");
      statement.bind_text(1, "Markov Chains");
      if (statement.step() == SQLITE_ROW) {
        return {
            .operation = domain::ProposalOperation::update_article,
            .article_id = domain::ArticleId{statement.text(0)}};
      }
      return {};
    } catch (const storage::StorageError& error) {
      throw CompilerError(CompilerErrorKind::state_error,
                          "could not read article state: " +
                              std::string(error.what()));
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
      const std::optional<domain::ArticleId>& article_id) const {
    try {
      storage::Database database(state_path());
      Transaction transaction(database);

      // A newer proposal replaces older review work for the same MVP article.
      Statement supersede(
          database.native_handle(),
          article_id
              ? "UPDATE proposals SET status = 'superseded' "
                "WHERE article_id = ? AND status IN ('pending', 'approved')"
              : "UPDATE proposals SET status = 'superseded' "
                "WHERE article_id IS NULL AND operation = 'create_article' "
                "AND status IN ('pending', 'approved')");
      if (article_id) {
        supersede.bind_text(1, article_id->value);
      }
      static_cast<void>(supersede.step());

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
