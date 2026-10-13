#include "kc/updates/article_updates.hpp"

#include "kc/domain/concept.hpp"
#include "kc/domain/json.hpp"
#include "kc/domain/markdown.hpp"
#include "kc/domain/path.hpp"
#include "kc/import/sha256.hpp"
#include "kc/storage/database.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace kc::updates {
namespace {
/// Parameterized, read-only queries keep identifiers out of SQL text.
class Query {
 public:
  Query(sqlite3* db, const char* sql, const std::string& id) {
    if (sqlite3_prepare_v2(db, sql, -1, &row_, nullptr) != SQLITE_OK) {
      throw storage::StorageError(sqlite3_errmsg(db));
    }
    if (sqlite3_bind_text(row_, 1, id.data(), static_cast<int>(id.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      sqlite3_finalize(row_);
      throw storage::StorageError(sqlite3_errmsg(db));
    }
  }
  ~Query() { sqlite3_finalize(row_); }
  Query(const Query&) = delete;
  Query& operator=(const Query&) = delete;
  bool next() {
    const auto status = sqlite3_step(row_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw storage::StorageError(sqlite3_errmsg(sqlite3_db_handle(row_)));
    }
    return status == SQLITE_ROW;
  }
  std::string text(int column) const {
    const auto* value = sqlite3_column_text(row_, column);
    return value ? std::string(reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(row_, column))) : "";
  }
 private:
  sqlite3_stmt* row_{nullptr};
};

domain::ArticleProposal parse(const std::string& text) {
  try {
    const auto parsed = domain::parse_article_proposal(nlohmann::json::parse(text));
    if (parsed) return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    // Corrupt history must produce the same actionable failure as bad shape.
  }
  throw std::invalid_argument("applied structured content is invalid; restore article history before updating");
}

void merge_citations(std::vector<domain::Citation>& retained,
                     const std::vector<domain::Citation>& incoming) {
  for (const auto& citation : incoming) {
    if (std::ranges::none_of(retained, [&](const auto& old) {
          // Normalized rows use page/start as their citation slot. Retain the
          // already approved span if new evidence uses that same slot.
          return old.page_id == citation.page_id &&
                 old.start_char == citation.start_char;
        })) {
      retained.push_back(citation);
    }
  }
}

/// Review claims individually; a removed block includes the old heading and
/// citations, so regeneration cannot disguise removal through section grouping.
nlohmann::json claims(const domain::ArticleProposal& proposal) {
  auto result = nlohmann::json::array();
  const nlohmann::json document = proposal;
  for (const auto& section : document["sections"]) {
    for (const auto& block : section["blocks"]) {
      result.push_back({{"section_key", section["key"]},
                        {"heading", section["heading"]}, {"block", block}});
    }
  }
  for (const auto& related : document["related_concepts"]) {
    result.push_back({{"related_concept", related}});
  }
  return result;
}

/// Match a complete retained claim, allowing extra citations for the same text.
/// Heading changes are visible removals/additions rather than hidden rewrites.
bool contains_claim(const nlohmann::json& items, const nlohmann::json& claim) {
  for (auto candidate : items) {
    auto original = claim;
    if (candidate.contains("block") != claim.contains("block")) continue;
    const auto field = claim.contains("block") ? "block" : "related_concept";
    const auto citations = original[field]["citations"];
    const auto candidate_citations = candidate[field]["citations"];
    original[field].erase("citations");
    candidate[field].erase("citations");
    if (candidate != original) continue;
    if (std::ranges::all_of(citations, [&](const auto& citation) {
          return std::ranges::find(candidate_citations, citation) != candidate_citations.end();
        })) return true;
  }
  return false;
}
}  // namespace

std::string managed_region(std::string_view content, std::string_view section_id) {
  const auto start = "<!-- kc:managed:start id=\"" + std::string(section_id) + "\" -->";
  constexpr std::string_view end = "<!-- kc:managed:end -->";
  const auto first = content.find(start);
  const auto last = content.find(end);
  if (first == std::string_view::npos || last == std::string_view::npos ||
      last < first + start.size() || content.find(start, first + 1) != std::string_view::npos ||
      content.find(end, last + 1) != std::string_view::npos) {
    throw std::invalid_argument("article ownership markers changed; restore the managed region and compile again");
  }
  return std::string(content.substr(first + start.size(), last - first - start.size()));
}

ArticleBase load_base(sqlite3* connection, const std::filesystem::path& root,
    const domain::ProjectConfig& config, const domain::ArticleId& article_id) {
  Query row(connection,
      "SELECT r.apply_run_id, p.payload_json, r.managed_sha256, r.new_content_sha256, a.vault_path "
      "FROM apply_runs r JOIN proposals p ON p.proposal_id = r.proposal_id "
      "JOIN articles a ON a.article_id = r.article_id WHERE r.article_id = ? "
      "ORDER BY r.rowid DESC LIMIT 1", article_id.value);
  if (!row.next()) {
    throw std::invalid_argument("article has no applied structured history; restore history before updating");
  }
  const auto path = root / domain::path_from_utf8(row.text(4));
  if (!domain::is_within(root / config.paths.vault, path) ||
      std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("base article is missing or outside the vault; restore it before updating");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::invalid_argument("base article cannot be read");
  const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) throw std::invalid_argument("base article could not be read completely");
  const auto hash = source_import::sha256_text(managed_region(content, config.vault.generated_section_id));
  const auto expected = row.text(2);
  if ((!expected.empty() && hash != expected) ||
      (expected.empty() && source_import::sha256_text(content) != row.text(3))) {
    throw std::invalid_argument("managed article content changed; restore the approved managed region, then compile and review again (keep manual notes outside the markers)");
  }
  auto proposal = parse(row.text(1));
  // A create payload does not yet carry the article ID assigned during apply.
  proposal.operation = domain::ProposalOperation::update_article;
  proposal.article.article_id = article_id;
  return {{row.text(0)}, std::move(proposal), hash};
}

domain::ArticleProposal accumulate(const domain::ArticleProposal& prior,
                                   const domain::ArticleProposal& additions) {
  auto result = prior;
  result.article = additions.article;
  for (const auto& section : additions.sections) {
    for (const auto& block : section.blocks) {
      domain::ProposalBlock* retained = nullptr;
      for (auto& old_section : result.sections) {
        if (old_section.key != section.key) continue;
        for (auto& old_block : old_section.blocks) {
          if (old_block.kind == block.kind &&
              domain::markdown_literal(old_block.text) == domain::markdown_literal(block.text)) {
            retained = &old_block;
            break;
          }
        }
        if (retained) break;
      }
      if (retained) {
        merge_citations(retained->citations, block.citations);
      } else {
        // Include preceding additions in duplicate detection within this batch.
        auto target = std::ranges::find_if(result.sections, [&](const auto& old) {
          return old.key == section.key && old.heading == section.heading;
        });
        if (target == result.sections.end()) {
          result.sections.push_back({section.key, section.heading, {block}});
        } else {
          target->blocks.push_back(block);
        }
      }
    }
  }
  for (const auto& related : additions.related_concepts) {
    auto retained = std::ranges::find_if(result.related_concepts, [&](const auto& old) {
      return domain::concept_identity_key(old.title) == domain::concept_identity_key(related.title) &&
             domain::markdown_literal(old.reason) == domain::markdown_literal(related.reason);
    });
    if (retained == result.related_concepts.end()) {
      result.related_concepts.push_back(related);
    } else {
      merge_citations(retained->citations, related.citations);
    }
  }
  return result;
}

std::optional<nlohmann::json> review_changes(sqlite3* connection,
    const domain::ProposalId& proposal_id, const domain::ArticleProposal& proposal) {
  if (proposal.operation != domain::ProposalOperation::update_article) return std::nullopt;
  Query row(connection,
      "SELECT b.base_apply_run_id, b.managed_sha256, b.update_mode, p.payload_json, r.article_id "
      "FROM proposal_update_bases b JOIN apply_runs r ON r.apply_run_id = b.base_apply_run_id "
      "JOIN proposals p ON p.proposal_id = r.proposal_id WHERE b.proposal_id = ?", proposal_id.value);
  if (!row.next()) {
    Query legacy(connection, "SELECT status FROM proposals WHERE proposal_id = ?", proposal_id.value);
    if (legacy.next() && legacy.text(0) != "pending" && legacy.text(0) != "approved") {
      // Historical records remain inspectable; an unknown base is never
      // reconstructed or presented as a verified removal-free update.
      return nlohmann::json{{"mode", "legacy_unverified"}, {"base_apply_run_id", nullptr},
          {"base_managed_sha256", nullptr}, {"retained_claim_count", nullptr},
          {"removed_claims", nullptr}, {"added_claims", nullptr}};
    }
    throw std::invalid_argument("update proposal has no base revision; compile and review a new proposal");
  }
  if (!proposal.article.article_id || proposal.article.article_id->value != row.text(4)) {
    throw std::invalid_argument("update base belongs to a different article");
  }
  const auto before = claims(parse(row.text(3)));
  const auto after = claims(proposal);
  auto removed = nlohmann::json::array();
  auto added = nlohmann::json::array();
  for (const auto& claim : before) {
    if (!contains_claim(after, claim)) removed.push_back(claim);
  }
  for (const auto& claim : after) {
    if (!contains_claim(before, claim)) added.push_back(claim);
  }
  if (row.text(2) == "additive" && !removed.empty()) {
    throw std::invalid_argument("additive update discards approved content; compile again");
  }
  return nlohmann::json{{"mode", row.text(2)}, {"base_apply_run_id", row.text(0)},
      {"base_managed_sha256", row.text(1)}, {"retained_claim_count", before.size() - removed.size()},
      {"removed_claims", removed}, {"added_claims", added}};
}

void validate_base(sqlite3* connection, const std::filesystem::path& root,
    const domain::ProjectConfig& config, const domain::ProposalId& proposal_id,
    const domain::ArticleProposal& proposal) {
  const auto changes = review_changes(connection, proposal_id, proposal);
  if (!changes) return;
  if (changes->at("mode") == "legacy_unverified") {
    throw std::invalid_argument("update proposal has no base revision; compile and review a new proposal");
  }
  const auto base = load_base(connection, root, config, *proposal.article.article_id);
  if (base.apply_run_id.value != changes->at("base_apply_run_id").get<std::string>() ||
      base.managed_sha256 != changes->at("base_managed_sha256").get<std::string>()) {
    throw std::invalid_argument("base article revision changed; compile and review a new proposal");
  }
}
}  // namespace kc::updates
