#include "kc/vault/vault_writer.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"
#include "kc/domain/json.hpp"
#include "kc/domain/markdown.hpp"
#include "kc/import/sha256.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/concept_identity.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace kc::vault {
namespace {

/// RAII wrapper for the vault layer's parameterized SQLite statements.
class Statement {
public:
  Statement(sqlite3 *connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_,
                           nullptr) != SQLITE_OK) {
      throw VaultError(VaultErrorKind::state_error,
                       "failed to prepare vault state query: " +
                           std::string(sqlite3_errmsg(connection_)));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  void bind_text(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw VaultError(VaultErrorKind::state_error,
                       "failed to bind vault state query: " +
                           std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw VaultError(VaultErrorKind::state_error,
                       "failed to bind vault state query: " +
                           std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_null(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw VaultError(VaultErrorKind::state_error,
                       "failed to bind vault state query: " +
                           std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw VaultError(VaultErrorKind::state_error,
                       "vault state query failed: " +
                           std::string(sqlite3_errmsg(connection_)));
    }
    return status;
  }

  [[nodiscard]] std::string text(const int column) const {
    const auto *value = sqlite3_column_text(statement_, column);
    return value == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char *>(value));
  }

  [[nodiscard]] std::int64_t integer(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] bool is_null(const int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

private:
  sqlite3 *connection_;
  sqlite3_stmt *statement_{nullptr};
};

/// Roll back database changes unless commit has completed.
class Transaction {
public:
  explicit Transaction(storage::Database &database) : database_(database) {
    database_.execute("BEGIN IMMEDIATE;");
  }

  ~Transaction() {
    if (!committed_) {
      try {
        database_.execute("ROLLBACK;");
      } catch (...) {
        // Preserve the primary application error.
      }
    }
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  void commit() {
    database_.execute("COMMIT;");
    committed_ = true;
  }

private:
  storage::Database &database_;
  bool committed_{false};
};

struct ArticleRecord {
  domain::ArticleId article_id;
  std::string title;
  std::string slug;
  std::filesystem::path vault_path;
};

struct ProposalRecord {
  domain::ProposalId proposal_id;
  std::optional<domain::ArticleId> article_id;
  domain::ProposalOperation operation{
      domain::ProposalOperation::create_article};
  std::string status;
  domain::ArticleProposal proposal;
};

/// Metadata required to create a readable, immutable source footnote.
struct SourceReference {
  domain::PageId page_id;
  domain::SourceVersionId source_version_id;
  domain::SourceId source_id;
  std::uint32_t page_number{1};
  domain::SourceKind source_kind{domain::SourceKind::text};
  std::string display_name;
  std::string original_filename;
  std::filesystem::path stored_path;
  domain::Sha256 source_sha256;
  std::filesystem::path vault_source_path;
};

struct CitationState {
  std::unordered_map<std::string, SourceReference> pages;
  std::unordered_set<std::string> normalized_rows;
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

bool is_safe_relative(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(
      path, [](const auto &component) { return component == ".."; });
}

/// Verify a path remains inside a trusted root after resolving symlinked
/// parents. This prevents a replaced vault/cache directory from redirecting a
/// write outside the initialized project.
bool is_within(const std::filesystem::path &trusted_root,
               const std::filesystem::path &candidate) {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(trusted_root, error);
  if (error) {
    return false;
  }
  const auto resolved = std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    return false;
  }
  const auto relative = resolved.lexically_relative(root);
  return !relative.empty() && !relative.is_absolute() &&
         std::ranges::none_of(
             relative, [](const auto &component) { return component == ".."; });
}

domain::ProjectConfig read_config(const std::filesystem::path &root) {
  std::ifstream input(root / "kc.json");
  if (!input) {
    throw VaultError(VaultErrorKind::invalid_project,
                     "no readable kc.json at project root");
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      const auto detail = parsed.issues.empty() ? "unknown validation error"
                                                : parsed.issues.front().message;
      throw VaultError(VaultErrorKind::invalid_project,
                       "invalid project configuration: " + detail);
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception &) {
    throw VaultError(VaultErrorKind::invalid_project,
                     "project configuration is not valid JSON");
  }
}

domain::ProposalOperation parse_operation(const std::string_view operation) {
  if (operation == "create_article") {
    return domain::ProposalOperation::create_article;
  }
  if (operation == "update_article") {
    return domain::ProposalOperation::update_article;
  }
  throw VaultError(VaultErrorKind::state_error,
                   "proposal has an unknown operation");
}

domain::SourceKind parse_source_kind(const std::string_view kind) {
  if (kind == "pdf") {
    return domain::SourceKind::pdf;
  }
  if (kind == "markdown") {
    return domain::SourceKind::markdown;
  }
  if (kind == "text") {
    return domain::SourceKind::text;
  }
  throw VaultError(VaultErrorKind::state_error,
                   "citation source has an unknown source kind");
}

ProposalRecord load_proposal(sqlite3 *connection,
                             const domain::ProposalId &proposal_id) {
  Statement statement(
      connection,
      "SELECT article_id, operation, payload_json, status FROM proposals "
      "WHERE proposal_id = ?");
  statement.bind_text(1, proposal_id.value);
  if (statement.step() != SQLITE_ROW) {
    throw VaultError(VaultErrorKind::proposal_not_found,
                     "proposal does not exist: " + proposal_id.value);
  }

  ProposalRecord record{.proposal_id = proposal_id,
                        .operation = parse_operation(statement.text(1)),
                        .status = statement.text(3)};
  if (!statement.is_null(0)) {
    record.article_id = domain::ArticleId{statement.text(0)};
  }

  try {
    const auto document = nlohmann::json::parse(statement.text(2));
    const auto parsed = domain::parse_article_proposal(document);
    if (!parsed) {
      const auto detail = parsed.issues.empty()
                              ? "unknown proposal validation error"
                              : parsed.issues.front().path + ": " +
                                    parsed.issues.front().message;
      throw VaultError(VaultErrorKind::validation_failed,
                       "stored proposal is invalid: " + detail);
    }
    record.proposal = *parsed.value;
  } catch (const nlohmann::json::exception &) {
    throw VaultError(VaultErrorKind::validation_failed,
                     "stored proposal payload is not valid JSON");
  }

  if (record.proposal.operation != record.operation) {
    throw VaultError(VaultErrorKind::validation_failed,
                     "proposal payload operation does not match state");
  }
  return record;
}

std::optional<ArticleRecord> load_article(sqlite3 *connection,
                                          const domain::ArticleId &article_id) {
  Statement statement(
      connection, "SELECT article_id, title, slug, vault_path FROM articles "
                  "WHERE article_id = ?");
  statement.bind_text(1, article_id.value);
  if (statement.step() != SQLITE_ROW) {
    return std::nullopt;
  }
  return ArticleRecord{.article_id = {statement.text(0)},
                       .title = statement.text(1),
                       .slug = statement.text(2),
                       .vault_path = domain::path_from_utf8(statement.text(3))};
}

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

/// Serialize a normalized citation row into an unambiguous set key.
std::string citation_key(const std::string_view section_key,
                         const std::size_t block_index,
                         const domain::Citation &citation) {
  std::string key;
  key.reserve(section_key.size() + citation.page_id.value.size() +
              citation.quote.size() + 64U);
  key += section_key;
  key.push_back('\0');
  key += std::to_string(block_index);
  key.push_back('\0');
  key += citation.page_id.value;
  key.push_back('\0');
  key += std::to_string(citation.start_char);
  key.push_back('\0');
  key += std::to_string(citation.end_char);
  key.push_back('\0');
  key += citation.quote;
  return key;
}

std::filesystem::path source_vault_filename(const SourceReference &source) {
  auto extension = domain::path_from_utf8(source.original_filename).extension();
  if (extension.empty()) {
    switch (source.source_kind) {
    case domain::SourceKind::pdf:
      extension = ".pdf";
      break;
    case domain::SourceKind::markdown:
      extension = ".md";
      break;
    case domain::SourceKind::text:
      extension = ".txt";
      break;
    }
  }
  // Both IDs make the destination immutable across later source versions.
  return domain::path_from_utf8(source.source_id.value + "-" +
                               source.source_version_id.value +
                               domain::path_to_utf8(extension));
}

CitationState load_citation_state(sqlite3 *connection,
                                  const ProposalRecord &proposal,
                                  const domain::ProjectConfig &config) {
  Statement statement(
      connection,
      "SELECT pc.section_key, pc.block_index, pc.page_id, pc.start_char, "
      "pc.end_char, pc.quote, p.page_number, p.text, "
      "v.source_version_id, v.sha256, v.original_filename, v.stored_path, "
      "s.source_id, s.display_name, s.source_kind "
      "FROM proposal_citations pc "
      "JOIN source_pages p ON p.page_id = pc.page_id "
      "JOIN source_versions v ON v.source_version_id = p.source_version_id "
      "JOIN sources s ON s.source_id = v.source_id "
      "WHERE pc.proposal_id = ? "
      "ORDER BY pc.section_key, pc.block_index, pc.page_id, pc.start_char");
  statement.bind_text(1, proposal.proposal_id.value);

  CitationState state;
  while (statement.step() == SQLITE_ROW) {
    const auto start = static_cast<std::size_t>(statement.integer(3));
    const auto end = static_cast<std::size_t>(statement.integer(4));
    const auto page_text = statement.text(7);
    const auto quote = statement.text(5);
    if (start >= end || end > page_text.size() ||
        normalize_whitespace(page_text.substr(start, end - start)) != quote) {
      throw VaultError(
          VaultErrorKind::validation_failed,
          "normalized proposal citation no longer matches its page");
    }

    const domain::Citation citation{.page_id = {statement.text(2)},
                                    .start_char = start,
                                    .end_char = end,
                                    .quote = quote};
    state.normalized_rows.insert(
        citation_key(statement.text(0),
                     static_cast<std::size_t>(statement.integer(1)), citation));

    SourceReference source{.page_id = citation.page_id,
                           .source_version_id = {statement.text(8)},
                           .source_id = {statement.text(12)},
                           .page_number =
                               static_cast<std::uint32_t>(statement.integer(6)),
                           .source_kind = parse_source_kind(statement.text(14)),
                           .display_name = statement.text(13),
                           .original_filename = statement.text(10),
                           .stored_path = statement.text(11),
                           .source_sha256 = statement.text(9)};
    if (!is_safe_relative(source.stored_path)) {
      throw VaultError(VaultErrorKind::state_error,
                       "citation source path is not project-relative");
    }
    source.vault_source_path =
        config.vault.source_directory / source_vault_filename(source);

    const auto [found, inserted] =
        state.pages.emplace(source.page_id.value, std::move(source));
    if (!inserted &&
        (found->second.source_version_id.value != statement.text(8) ||
         found->second.page_number !=
             static_cast<std::uint32_t>(statement.integer(6)))) {
      throw VaultError(VaultErrorKind::state_error,
                       "citation page resolves to inconsistent source state");
    }
  }
  return state;
}

/// Ensure the immutable normalized rows still correspond exactly to citations
/// in the approved proposal payload.
void validate_normalized_citations(const ProposalRecord &proposal,
                                   const CitationState &state) {
  std::unordered_set<std::string> expected;
  std::unordered_map<std::string, std::size_t> next_block_index;
  for (const auto &section : proposal.proposal.sections) {
    const auto section_key = std::string(domain::to_string(section.key));
    for (const auto &block : section.blocks) {
      const auto block_index = next_block_index[section_key]++;
      for (const auto &citation : block.citations) {
        expected.insert(citation_key(section_key, block_index, citation));
      }
    }
  }
  for (const auto &related : proposal.proposal.related_concepts) {
    const auto block_index = next_block_index["related_concepts"]++;
    for (const auto &citation : related.citations) {
      expected.insert(citation_key("related_concepts", block_index, citation));
    }
  }

  if (expected.empty() || expected != state.normalized_rows) {
    throw VaultError(
        VaultErrorKind::validation_failed,
        "proposal citations do not match their normalized validated records");
  }
}

std::string
citation_references(const std::vector<domain::Citation> &citations) {
  std::string rendered;
  std::unordered_set<std::string> seen;
  for (const auto &citation : citations) {
    if (seen.insert(citation.page_id.value).second) {
      rendered += " [^" + citation.page_id.value + "]";
    }
  }
  return rendered;
}

std::string encode_markdown_url(const std::string_view value) {
  constexpr std::string_view hexadecimal = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9')) || character == '/' || character == '-' ||
        character == '_' || character == '.' || character == '~') {
      encoded.push_back(character);
    } else {
      encoded.push_back('%');
      encoded.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
      encoded.push_back(hexadecimal[byte & 0x0fU]);
    }
  }
  return encoded;
}

/// JSON double-quoted strings are YAML-compatible and retain string types,
/// including empty, boolean-looking, null-looking, and numeric-looking values.
std::string yaml_scalar(const std::string &value) {
  return nlohmann::json(value).dump();
}

/// yaml-cpp exposes an implicit scalar's bytes without resolving its type.
/// Recognize YAML 1.1/1.2 ambiguous scalars so metadata readers cannot disagree
/// with the string-valued search index. Quoted or explicitly !!str values are
/// always strings; ordinary legacy plain aliases remain supported.
bool yaml_string(const YAML::Node& value) {
  if (!value.IsScalar()) {
    return false;
  }
  if (value.Tag() == "!" || value.Tag() == "tag:yaml.org,2002:str") {
    return true;
  }
  if (value.Tag() != "?") {
    return false;
  }
  static const std::regex typed(
      R"(^([~]|null|true|false|yes|no|on|off|[-+]?(0[xob][0-9a-f_]+|[0-9][0-9_]*(:[0-5]?[0-9])+(\.[0-9_]*)?|([0-9][0-9_]*(\.[0-9_]*)?|\.[0-9_]+)(e[-+]?[0-9]+)?|\.(inf|nan))|[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}.*)$)",
      std::regex::icase);
  return !value.Scalar().empty() && !std::regex_match(value.Scalar(), typed);
}

/// Read effective user-owned aliases from the final article, without rewriting
/// frontmatter. Legacy `aliases:` null is treated as an empty list. Ambiguous
/// YAML or changed compiler identity aborts before filesystem/state publication.
std::vector<std::string> article_aliases(const std::string& content,
                                        const domain::ArticleId& article_id,
                                        const std::string& title) {
  try {
    std::istringstream input(content);
    std::string line;
    const auto next_line = [&]() {
      if (!std::getline(input, line)) {
        return false;
      }
      if (line.ends_with('\r')) {
        line.pop_back();
      }
      return true;
    };
    if (!next_line() || line != "---") {
      throw std::invalid_argument("article must begin with YAML frontmatter");
    }
    std::string yaml;
    bool closed = false;
    while (next_line()) {
      if (line == "---") {
        closed = true;
        break;
      }
      yaml += line + "\n";
    }
    if (!closed) {
      throw std::invalid_argument("article frontmatter is not closed");
    }
    const auto documents = YAML::LoadAll(yaml);
    if (documents.size() != 1U || !documents.front().IsMap()) {
      throw std::invalid_argument("article frontmatter must be one YAML mapping");
    }
    const auto metadata = documents.front();
    std::unordered_set<std::string> keys;
    for (const auto& entry : metadata) {
      if (!entry.first.IsScalar() || entry.first.Scalar() == "<<" ||
          !keys.insert(entry.first.Scalar()).second) {
        throw std::invalid_argument("article frontmatter keys must be unique scalars");
      }
    }
    if (metadata["kc_schema"].as<int>() != 1 ||
        !yaml_string(metadata["article_id"]) ||
        metadata["article_id"].Scalar() != article_id.value ||
        !yaml_string(metadata["title"]) || metadata["title"].Scalar() != title) {
      throw std::invalid_argument("compiler-owned frontmatter identity has changed");
    }
    std::vector<std::string> aliases;
    const auto values = metadata["aliases"];
    if (!values || values.IsNull()) {
      return aliases;
    }
    if (!values.IsSequence()) {
      throw std::invalid_argument("aliases must be a YAML list of scalar values");
    }
    for (const auto& value : values) {
      if (!yaml_string(value)) {
        throw std::invalid_argument("aliases must be YAML strings; quote boolean, null, numeric, or date-like values");
      }
      // YAML scalar bytes, rather than proposal aliases, determine the index.
      // Validate Unicode and line structure using the same metadata policy.
      domain::ArticleProposal descriptor;
      descriptor.article.title = title;
      descriptor.article.aliases = {value.Scalar()};
      if (!domain::validate_markdown(descriptor)) {
        throw std::invalid_argument("aliases must contain safe single-line text");
      }
      aliases.push_back(value.Scalar());
    }
    return aliases;
  } catch (const YAML::Exception& error) {
    throw VaultError(VaultErrorKind::validation_failed,
                     "invalid article frontmatter: " + std::string(error.what()));
  } catch (const std::invalid_argument& error) {
    throw VaultError(VaultErrorKind::validation_failed, error.what());
  }
}

std::string render_managed_content(const domain::ArticleProposal &proposal,
                                   const CitationState &citations) {
  std::ostringstream output;
  bool rendered_related_section = false;

  for (const auto &section : proposal.sections) {
    output << "## " << domain::markdown_literal(section.heading) << "\n\n";
    for (const auto &block : section.blocks) {
      if (block.kind == domain::BlockKind::bullet) {
        output << "- ";
      }
      output << domain::markdown_literal(block.text) << citation_references(block.citations) << "\n\n";
    }

    if (section.key == domain::SectionKey::related_concepts) {
      rendered_related_section = true;
      for (const auto &related : proposal.related_concepts) {
        output << "- [[" << related.title << "]]";
        if (!related.reason.empty()) {
          output << " — " << domain::markdown_literal(related.reason);
        }
        output << citation_references(related.citations) << "\n";
      }
      if (!proposal.related_concepts.empty()) {
        output << '\n';
      }
    }
  }

  if (!rendered_related_section) {
    output << "## Related concepts\n\n";
    for (const auto &related : proposal.related_concepts) {
      output << "- [[" << related.title << "]]";
      if (!related.reason.empty()) {
        output << " — " << domain::markdown_literal(related.reason);
      }
      output << citation_references(related.citations) << "\n";
    }
    output << '\n';
  }

  output << "## Sources\n\n";
  std::set<std::string> ordered_page_ids;
  for (const auto &section : proposal.sections) {
    for (const auto &block : section.blocks) {
      for (const auto &citation : block.citations) {
        ordered_page_ids.insert(citation.page_id.value);
      }
    }
  }
  for (const auto &related : proposal.related_concepts) {
    for (const auto &citation : related.citations) {
      ordered_page_ids.insert(citation.page_id.value);
    }
  }

  for (const auto &page_id : ordered_page_ids) {
    const auto found = citations.pages.find(page_id);
    if (found == citations.pages.end()) {
      throw VaultError(VaultErrorKind::validation_failed,
                       "proposal citation has no source metadata: " + page_id);
    }
    const auto &source = found->second;
    auto label = domain::markdown_literal(source.display_name);
    if (source.source_kind == domain::SourceKind::pdf) {
      label += ", p. " + std::to_string(source.page_number);
    }
    auto target =
        encode_markdown_url(domain::path_to_utf8(source.vault_source_path));
    if (source.source_kind == domain::SourceKind::pdf) {
      target += "#page=" + std::to_string(source.page_number);
    }
    output << "[^" << page_id << "]: [" << label << "](" << target << ")\n";
  }

  auto rendered = output.str();
  while (rendered.ends_with('\n')) {
    rendered.pop_back();
  }
  return rendered;
}

std::string render_new_article(const domain::ArticleProposal &proposal,
                               const domain::ArticleId &article_id,
                               const std::string_view start_marker,
                               const std::string_view end_marker,
                               const std::string &managed_content) {
  std::ostringstream output;
  output << "---\n"
         << "kc_schema: 1\n"
         << "article_id: " << yaml_scalar(article_id.value) << "\n"
         << "title: " << yaml_scalar(proposal.article.title) << "\n"
         << (proposal.article.aliases.empty() ? "aliases: []\n" : "aliases:\n");
  for (const auto &alias : proposal.article.aliases) {
    output << "  - " << yaml_scalar(alias) << "\n";
  }
  output << "---\n\n"
         << "# " << domain::markdown_literal(proposal.article.title) << "\n\n"
         << start_marker << "\n\n"
         << managed_content << "\n\n"
         << end_marker << "\n";
  return output.str();
}

std::string replace_managed_content(const std::string &existing,
                                    const std::string_view start_marker,
                                    const std::string_view end_marker,
                                    const std::string &managed_content) {
  const auto start = existing.find(start_marker);
  if (start == std::string::npos ||
      existing.find(start_marker, start + start_marker.size()) !=
          std::string::npos) {
    throw VaultError(
        VaultErrorKind::unsafe_write,
        "existing article must contain exactly one managed start marker");
  }
  const auto managed_start = start + start_marker.size();
  const auto end = existing.find(end_marker, managed_start);
  if (end == std::string::npos ||
      existing.find(end_marker, end + end_marker.size()) != std::string::npos) {
    throw VaultError(
        VaultErrorKind::unsafe_write,
        "existing article must contain exactly one managed end marker");
  }
  if (existing.find(end_marker) < start) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "existing article managed markers are out of order");
  }

  // Prefix and suffix include the original markers. Only bytes strictly
  // between them are replaced; all user-authored bytes remain untouched.
  return existing.substr(0, managed_start) + "\n\n" + managed_content + "\n\n" +
         existing.substr(end);
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "could not read existing article: " + path.string());
  }
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "existing article could not be read completely");
  }
  return content;
}

/// Check ownership on the final artifact as well as on incoming fields. A user
/// prefix/suffix must not introduce an extra ownership marker. Generated text
/// and source labels are escaped; the managed region must equal that rendering.
void validate_rendered_article(const std::string& content,
                               const std::string_view start_marker,
                               const std::string_view end_marker,
                               const std::string& managed_content) {
  const auto start = content.find(start_marker);
  const auto end = content.find(end_marker);
  if (start == std::string::npos || end == std::string::npos || end <= start ||
      content.substr(start + start_marker.size(),
                     end - start - start_marker.size()) !=
          "\n\n" + managed_content + "\n\n") {
    throw VaultError(VaultErrorKind::validation_failed,
                     "rendered article has invalid ownership boundaries");
  }
  // Count the reserved namespace, including markers with a different ID, so
  // injected user/source/model markup cannot change ownership interpretation.
  const auto first = content.find("kc:managed:");
  const auto second = content.find("kc:managed:", first + 1U);
  if (first != start + 5U || second != end + 5U ||
      content.find("kc:managed:", second + 1U) != std::string::npos) {
    throw VaultError(VaultErrorKind::validation_failed,
                     "rendered article contains additional reserved markers");
  }
  // A user footnote outside the managed region must not shadow a generated
  // source reference. Other user Markdown stays untouched.
  std::unordered_set<std::string> source_ids;
  std::istringstream generated(managed_content);
  std::string line;
  const std::regex definition(R"(^ {0,3}\[\^(pg_[0-9A-HJKMNP-TV-Z]{26})\]:)");
  std::smatch match;
  while (std::getline(generated, line)) {
    if (std::regex_search(line, match, definition)) {
      source_ids.insert(match[1].str());
    }
  }
  std::unordered_set<std::string> seen;
  std::istringstream article(content);
  while (std::getline(article, line)) {
    if (std::regex_search(line, match, definition) &&
        source_ids.contains(match[1].str()) && !seen.insert(match[1].str()).second) {
      throw VaultError(VaultErrorKind::validation_failed,
                       "user footnote shadows a generated source reference");
    }
  }
}

/// Replace `destination` with a complete sibling temporary file.
///
/// POSIX rename replaces an existing destination atomically. The C++
/// `filesystem::rename` wrapper does not provide that behavior on Windows, so
/// use the native replace flag there. Because callers always create the
/// temporary file beside the destination, this remains a same-volume rename.
void replace_with_temporary_file(const std::filesystem::path &temporary,
                                 const std::filesystem::path &destination) {
#if defined(_WIN32)
  constexpr DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
  if (MoveFileExW(temporary.c_str(), destination.c_str(), flags) == 0) {
    const auto error = std::error_code(static_cast<int>(GetLastError()),
                                       std::system_category());
    throw VaultError(VaultErrorKind::unsafe_write,
                     "could not atomically replace vault file: " +
                         error.message());
  }
#else
  std::filesystem::rename(temporary, destination);
#endif
}

/// Write a complete sibling and atomically replace the destination with it.
void atomic_write(const std::filesystem::path &path,
                  const std::string_view content) {
  auto temporary = path;
  temporary += ".tmp-" + domain::generate_prefixed_ulid("");
  try {
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) {
        throw VaultError(VaultErrorKind::unsafe_write,
                         "could not create temporary vault file");
      }
      output.write(content.data(),
                   static_cast<std::streamsize>(content.size()));
      output.flush();
      if (!output) {
        throw VaultError(VaultErrorKind::unsafe_write,
                         "could not write complete temporary vault file");
      }
    }
    replace_with_temporary_file(temporary, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

void copy_source(const std::filesystem::path &project_root,
                 const std::filesystem::path &vault_root,
                 const SourceReference &source) {
  const auto retained = project_root / source.stored_path;
  const auto destination = vault_root / source.vault_source_path;
  if (!is_within(project_root, retained) ||
      !std::filesystem::is_regular_file(retained) ||
      std::filesystem::is_symlink(retained)) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "retained citation source is missing or unsafe");
  }
  if (source_import::sha256_file(retained) != source.source_sha256) {
    throw VaultError(
        VaultErrorKind::validation_failed,
        "retained citation source does not match its recorded hash");
  }
  if (!is_within(vault_root, destination)) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "configured source destination escapes the vault");
  }
  std::filesystem::create_directories(destination.parent_path());
  if (std::filesystem::exists(destination)) {
    if (!std::filesystem::is_regular_file(destination) ||
        std::filesystem::is_symlink(destination) ||
        source_import::sha256_file(destination) != source.source_sha256) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "vault source copy exists with different content");
    }
    return;
  }

  auto temporary = destination;
  temporary += ".tmp-" + domain::generate_prefixed_ulid("");
  try {
    std::filesystem::copy_file(retained, temporary);
    if (source_import::sha256_file(temporary) != source.source_sha256) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "copied citation source failed hash verification");
    }
    std::filesystem::rename(temporary, destination);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

std::filesystem::path create_backup(const std::filesystem::path &project_root,
                                    const domain::ProjectConfig &config,
                                    const domain::ArticleId &article_id,
                                    const domain::RunId &apply_run_id,
                                    const std::string &previous_content) {
  const auto relative = config.paths.cache / "backups" / article_id.value /
                        (apply_run_id.value + ".md");
  const auto path = project_root / relative;
  if (!is_safe_relative(relative) || !is_within(project_root, path)) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "configured backup path escapes the project");
  }
  std::filesystem::create_directories(path.parent_path());
  atomic_write(path, previous_content);
  return relative;
}

void persist_success(storage::Database &database, const ProposalRecord &record,
                     const domain::ArticleId &article_id,
                     const std::filesystem::path &vault_path,
                     const domain::RunId &apply_run_id,
                     const std::optional<domain::Sha256> &previous_sha256,
                     const domain::Sha256 &new_sha256,
                     const std::optional<std::filesystem::path> &backup_path,
                     const std::string &rendered_content,
                     const std::vector<std::string>& effective_aliases) {
  const auto timestamp = utc_now();

  if (record.operation == domain::ProposalOperation::create_article) {
    Statement insert(
        database.native_handle(),
        "INSERT INTO articles(article_id, title, slug, vault_path, "
        "content_sha256, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)");
    insert.bind_text(1, article_id.value);
    insert.bind_text(2, record.proposal.article.title);
    insert.bind_text(3, record.proposal.article.slug);
    insert.bind_text(4, domain::path_to_utf8(vault_path));
    insert.bind_text(5, new_sha256);
    insert.bind_text(6, timestamp);
    insert.bind_text(7, timestamp);
    static_cast<void>(insert.step());
  } else {
    Statement update(database.native_handle(),
                     "UPDATE articles SET content_sha256 = ?, updated_at = ? "
                     "WHERE article_id = ?");
    update.bind_text(1, new_sha256);
    update.bind_text(2, timestamp);
    update.bind_text(3, article_id.value);
    static_cast<void>(update.step());
    if (sqlite3_changes(database.native_handle()) != 1) {
      throw VaultError(VaultErrorKind::state_error,
                       "article disappeared while applying proposal");
    }
  }

  Statement delete_citations(
      database.native_handle(),
      "DELETE FROM article_citations WHERE article_id = ?");
  delete_citations.bind_text(1, article_id.value);
  static_cast<void>(delete_citations.step());

  Statement insert_citations(
      database.native_handle(),
      "INSERT INTO article_citations(article_id, section_key, block_index, "
      "page_id) SELECT DISTINCT ?, section_key, block_index, page_id "
      "FROM proposal_citations WHERE proposal_id = ?");
  insert_citations.bind_text(1, article_id.value);
  insert_citations.bind_text(2, record.proposal_id.value);
  static_cast<void>(insert_citations.step());

  Statement audit(
      database.native_handle(),
      "INSERT INTO apply_runs(apply_run_id, proposal_id, article_id, "
      "previous_content_sha256, new_content_sha256, backup_path, applied_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
  audit.bind_text(1, apply_run_id.value);
  audit.bind_text(2, record.proposal_id.value);
  audit.bind_text(3, article_id.value);
  if (previous_sha256) {
    audit.bind_text(4, *previous_sha256);
  } else {
    audit.bind_null(4);
  }
  audit.bind_text(5, new_sha256);
  if (backup_path) {
    audit.bind_text(6, backup_path->generic_string());
  } else {
    audit.bind_null(6);
  }
  audit.bind_text(7, timestamp);
  static_cast<void>(audit.step());

  Statement mark_applied(
      database.native_handle(),
      "UPDATE proposals SET article_id = ?, status = 'applied', applied_at = ? "
      "WHERE proposal_id = ? AND status = 'approved'");
  mark_applied.bind_text(1, article_id.value);
  mark_applied.bind_text(2, timestamp);
  mark_applied.bind_text(3, record.proposal_id.value);
  static_cast<void>(mark_applied.step());
  if (sqlite3_changes(database.native_handle()) != 1) {
    throw VaultError(VaultErrorKind::invalid_proposal_state,
                     "proposal is no longer approved");
  }

  // Indexing is intentionally last and is still inside the transaction. It
  // cannot become visible unless the article rename and every audit write have
  // already succeeded.
  Statement delete_index(database.native_handle(),
                         "DELETE FROM article_fts WHERE article_id = ?");
  delete_index.bind_text(1, article_id.value);
  static_cast<void>(delete_index.step());

  std::string aliases;
  for (const auto &alias : effective_aliases) {
    if (!aliases.empty()) {
      aliases.push_back('\n');
    }
    aliases += alias;
  }
  Statement insert_index(
      database.native_handle(),
      "INSERT INTO article_fts(article_id, title, aliases, body) "
      "VALUES (?, ?, ?, ?)");
  insert_index.bind_text(1, article_id.value);
  insert_index.bind_text(2, record.proposal.article.title);
  insert_index.bind_text(3, aliases);
  insert_index.bind_text(4, rendered_content);
  static_cast<void>(insert_index.step());
}

/// Approval and apply share the same identity checks. Canonicalize only the
/// effective update descriptor; immutable legacy proposal payloads stay intact.
void resolve_identity(sqlite3* connection, ProposalRecord& record) {
  if (record.article_id != record.proposal.article.article_id) {
    throw VaultError(VaultErrorKind::validation_failed,
                     "proposal article IDs do not match");
  }
  try {
    static_cast<void>(storage::resolve_concept(
        connection, record.proposal.article.title));
    record.proposal.article = storage::resolve_proposal_identity(
        connection, record.proposal);
  } catch (const storage::StorageError& error) {
    throw VaultError(VaultErrorKind::validation_failed, error.what());
  }
}

/// On case-sensitive filesystems, refuse sibling names that would alias the
/// destination on macOS/Windows. Existing aliases of the same physical file
/// are safe, including macOS's decomposed Unicode directory entries.
void validate_portable_destination(const std::filesystem::path& destination) {
  if (!std::filesystem::is_directory(destination.parent_path())) {
    return;
  }
  const auto utf8_name = destination.filename().u8string();
  const auto key = domain::concept_identity_key(std::string_view(
      reinterpret_cast<const char*>(utf8_name.data()), utf8_name.size()));
  for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path())) {
    const auto sibling = entry.path().filename().u8string();
    std::string sibling_key;
    try {
      sibling_key = domain::concept_identity_key(std::string_view(
          reinterpret_cast<const char*>(sibling.data()), sibling.size()));
      // Windows ignores trailing ASCII spaces/dots in ordinary filenames.
      while (!sibling_key.empty() &&
             (sibling_key.back() == ' ' || sibling_key.back() == '.')) {
        sibling_key.pop_back();
      }
    } catch (const std::invalid_argument&) {
      continue;  // An unrelated non-UTF-8 user filename cannot alias this title.
    }
    if (sibling_key == key && entry.path() != destination &&
        (!std::filesystem::exists(destination) ||
         !std::filesystem::equivalent(entry.path(), destination))) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "article filename collides under portable Unicode/case rules");
    }
  }
}

} // namespace

VaultWriter::VaultWriter(std::filesystem::path project_root)
    : project_root_(std::filesystem::absolute(std::move(project_root))
                        .lexically_normal()),
      config_(read_config(project_root_)) {
  if (!std::filesystem::is_regular_file(project_root_ / config_.paths.state)) {
    throw VaultError(
        VaultErrorKind::invalid_project,
        "project state database does not exist; run 'kc init' first");
  }
  const auto vault_root = project_root_ / config_.paths.vault;
  if (!std::filesystem::is_directory(vault_root) ||
      !is_within(project_root_, vault_root)) {
    throw VaultError(VaultErrorKind::invalid_project,
                     "configured vault is missing or escapes the project");
  }
  if (!domain::safe_managed_section_id(config_.vault.generated_section_id)) {
    throw VaultError(VaultErrorKind::invalid_project,
                     "generated section ID is not safe for a Markdown marker");
  }
}

void VaultWriter::approve(const domain::ProposalId &proposal_id) {
  try {
    storage::Database database(project_root_ / config_.paths.state);
    Transaction transaction(database);
    auto record = load_proposal(database.native_handle(), proposal_id);
    if (record.status != "pending") {
      throw VaultError(VaultErrorKind::invalid_proposal_state,
                       "only a pending proposal can be approved");
    }
    resolve_identity(database.native_handle(), record);
    Statement statement(
        database.native_handle(),
        "UPDATE proposals SET status = 'approved', reviewed_at = ? "
        "WHERE proposal_id = ? AND status = 'pending'");
    statement.bind_text(1, utc_now());
    statement.bind_text(2, proposal_id.value);
    static_cast<void>(statement.step());
    if (sqlite3_changes(database.native_handle()) != 1) {
      throw VaultError(VaultErrorKind::invalid_proposal_state,
                       "only a pending proposal can be approved");
    }
    transaction.commit();
  } catch (const storage::StorageError &error) {
    throw VaultError(VaultErrorKind::state_error,
                     "could not approve proposal: " +
                         std::string(error.what()));
  }
}

ApplyResult VaultWriter::apply(const domain::ProposalId &proposal_id,
                               const ApplyOptions &options) {
  try {
    storage::Database database(project_root_ / config_.paths.state);
    // Serialize approval-state validation, the filesystem replacement, and
    // durable state publication. A second process cannot race the same
    // approved proposal and later roll back over the first process's result.
    Transaction application_transaction(database);
    auto record = load_proposal(database.native_handle(), proposal_id);
    if (record.status != "approved") {
      throw VaultError(VaultErrorKind::invalid_proposal_state,
                       "only an approved proposal can be applied");
    }

    resolve_identity(database.native_handle(), record);

    const auto start_marker = "<!-- kc:managed:start id=\"" +
                              config_.vault.generated_section_id + "\" -->";
    constexpr std::string_view end_marker = "<!-- kc:managed:end -->";

    std::optional<ArticleRecord> existing_article;
    domain::ArticleId article_id;
    if (record.operation == domain::ProposalOperation::create_article) {
      if (record.article_id || record.proposal.article.article_id) {
        throw VaultError(
            VaultErrorKind::validation_failed,
            "create proposal must not identify an existing article");
      }
      article_id = domain::generate_article_id();
    } else {
      if (!record.article_id || !record.proposal.article.article_id ||
          *record.article_id != *record.proposal.article.article_id) {
        throw VaultError(VaultErrorKind::validation_failed,
                         "update proposal article IDs do not match");
      }
      existing_article =
          load_article(database.native_handle(), *record.article_id);
      if (!existing_article ||
          existing_article->title != record.proposal.article.title ||
          existing_article->slug != record.proposal.article.slug) {
        throw VaultError(VaultErrorKind::validation_failed,
                         "update proposal does not match its stored article");
      }
      article_id = *record.article_id;
    }

    const auto candidate_path = domain::concept_filename(record.proposal.article.title);
    const auto relative_vault_path = existing_article
                                         ? existing_article->vault_path
                                         : config_.paths.vault / candidate_path;
    if (!is_safe_relative(relative_vault_path)) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "article path is not project-relative");
    }

    try {
      storage::validate_concept_destination(database.native_handle(), relative_vault_path, article_id);
    } catch (const storage::StorageError& error) {
      throw VaultError(VaultErrorKind::validation_failed, error.what());
    }

    const auto vault_root = project_root_ / config_.paths.vault;
    const auto article_path = project_root_ / relative_vault_path;
    validate_portable_destination(article_path);
    if (!is_within(vault_root, article_path) ||
        std::filesystem::is_symlink(article_path)) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "article destination escapes the configured vault");
    }
    if (std::filesystem::exists(article_path) &&
        !std::filesystem::is_regular_file(article_path)) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "article destination is not a regular file");
    }

    const auto citation_state =
        load_citation_state(database.native_handle(), record, config_);
    validate_normalized_citations(record, citation_state);
    const auto managed_content =
        render_managed_content(record.proposal, citation_state);

    std::optional<std::string> previous_content;
    if (std::filesystem::exists(article_path)) {
      previous_content = read_file(article_path);
      if (!existing_article && !options.allow_overwrite_user_file) {
        throw VaultError(
            VaultErrorKind::unsafe_write,
            "refusing to overwrite an untracked user-authored file; use the "
            "explicit overwrite flag to approve this collision");
      }
    } else if (existing_article) {
      throw VaultError(VaultErrorKind::unsafe_write,
                       "tracked article file is missing");
    }

    std::string rendered_content;
    if (existing_article) {
      rendered_content = replace_managed_content(
          *previous_content, start_marker, end_marker, managed_content);
    } else {
      rendered_content =
          render_new_article(record.proposal, article_id, start_marker,
                             end_marker, managed_content);
    }

    // Check the completed article before copying sources, making backups, or
    // publishing state. User metadata remains byte-identical on updates.
    const auto effective_aliases = article_aliases(
        rendered_content, article_id, record.proposal.article.title);
    validate_rendered_article(rendered_content, start_marker, end_marker,
                              managed_content);

    // All cited sources are verified and copied before the article can expose
    // links to them. Version IDs prevent later imports from changing a target.
    for (const auto &[unused_page_id, source] : citation_state.pages) {
      static_cast<void>(unused_page_id);
      copy_source(project_root_, vault_root, source);
    }

    const auto apply_run_id = domain::generate_run_id();
    std::optional<std::filesystem::path> backup_path;
    std::optional<domain::Sha256> previous_sha256;
    if (previous_content) {
      previous_sha256 = source_import::sha256_text(*previous_content);
      backup_path = create_backup(project_root_, config_, article_id,
                                  apply_run_id, *previous_content);
    }
    const auto new_sha256 = source_import::sha256_text(rendered_content);

    atomic_write(article_path, rendered_content);
    try {
      persist_success(database, record, article_id, relative_vault_path,
                      apply_run_id, previous_sha256, new_sha256, backup_path,
                      rendered_content, effective_aliases);
      application_transaction.commit();
    } catch (...) {
      // Restore filesystem state if the post-write database transaction cannot
      // commit. The pre-write backup remains available for manual recovery.
      try {
        if (previous_content) {
          atomic_write(article_path, *previous_content);
        } else {
          std::error_code error;
          if (!std::filesystem::remove(article_path, error) || error) {
            throw VaultError(VaultErrorKind::unsafe_write,
                             "could not remove unrecorded new article");
          }
        }
      } catch (...) {
        throw VaultError(
            VaultErrorKind::unsafe_write,
            "database commit failed and automatic article recovery failed; "
            "restore the recorded backup manually");
      }
      throw;
    }

    return {.apply_run_id = apply_run_id,
            .proposal_id = proposal_id,
            .article_id = article_id,
            .vault_path = relative_vault_path,
            .backup_path = backup_path,
            .content_sha256 = new_sha256};
  } catch (const VaultError &) {
    throw;
  } catch (const storage::StorageError &error) {
    throw VaultError(VaultErrorKind::state_error,
                     "could not apply proposal state: " +
                         std::string(error.what()));
  } catch (const std::filesystem::filesystem_error &error) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "vault filesystem operation failed: " +
                         std::string(error.what()));
  } catch (const std::exception &error) {
    throw VaultError(VaultErrorKind::unsafe_write,
                     "vault application failed: " + std::string(error.what()));
  }
}

} // namespace kc::vault
