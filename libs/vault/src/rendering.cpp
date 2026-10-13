#include "kc/vault/rendering.hpp"
#include "kc/domain/markdown.hpp"
#include "kc/domain/path.hpp"
#include "kc/domain/text.hpp"
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cstdint>
#include <regex>
#include <set>
#include <sstream>

namespace kc::vault {
namespace {
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


std::vector<std::string> lines(std::string_view text) {
  std::vector<std::string> result;
  for (std::size_t start = 0; start < text.size();) {
    const auto end = text.find('\n', start);
    result.emplace_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start + 1));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}
}  // namespace

RenderedArticle render_article(const domain::ArticleProposal& proposal,
    const domain::ArticleId& article_id, const domain::VaultConfig& config,
    const CitationState& citations, const std::optional<std::string>& existing) {
  if (!domain::safe_managed_section_id(config.generated_section_id)) {
    throw VaultError(VaultErrorKind::validation_failed,"generated section ID is not safe for a Markdown marker");
  }
  if (existing && !domain::is_valid_source_text(*existing)) {
    throw VaultError(VaultErrorKind::validation_failed,"existing article must be UTF-8 without embedded NULs; preserve it and repair its encoding before review");
  }
  const auto marker = "<!-- kc:managed:start id=\"" + config.generated_section_id + "\" -->";
  constexpr std::string_view end = "<!-- kc:managed:end -->";
  const auto managed = render_managed_content(proposal, citations);
  auto content = existing ? replace_managed_content(*existing, marker, end, managed)
                          : render_new_article(proposal, article_id, marker, end, managed);
  auto aliases = article_aliases(content, article_id, proposal.article.title);
  validate_rendered_article(content, marker, end, managed);
  return {std::move(content), std::move(aliases)};
}

std::string rendered_diff(std::string_view before, std::string_view after,
                          std::string_view destination) {
  if (before == after) return {};
  const auto old = lines(before);
  const auto fresh = lines(after);
  std::ostringstream output;
  output << "--- " << (before.empty() ? "/dev/null" : "a/" + std::string(destination))
         << "\n+++ b/" << destination << "\n@@ -" << (old.empty() ? 0 : 1) << ',' << old.size()
         << " +" << (fresh.empty() ? 0 : 1) << ',' << fresh.size() << " @@\n";
  auto emit = [&](char prefix, const std::string& line) {
    output << prefix << line;
    if (!line.ends_with('\n')) output << "\n\\ No newline at end of file\n";
  };
  std::size_t prefix = 0;
  while (prefix < old.size() && prefix < fresh.size() && old[prefix] == fresh[prefix]) {
    emit(' ', old[prefix++]);
  }
  std::size_t suffix = 0;
  while (suffix < old.size() - prefix && suffix < fresh.size() - prefix &&
         old[old.size()-suffix-1] == fresh[fresh.size()-suffix-1]) ++suffix;
  const auto n = old.size() - prefix - suffix;
  const auto m = fresh.size() - prefix - suffix;
  constexpr std::size_t maximum_cells = 4'000'000;
  if (n + 1 > maximum_cells / (m + 1)) {
    for (std::size_t i = 0; i < n; ++i) emit('-', old[prefix+i]);
    for (std::size_t j = 0; j < m; ++j) emit('+', fresh[prefix+j]);
  } else {
    // Longest common subsequence retains unchanged lines inside a changed
    // region. The cap above bounds memory without dropping any diff bytes.
    std::vector<std::uint32_t> lengths((n+1)*(m+1), 0);
    auto cell = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return lengths[i*(m+1)+j]; };
    for (std::size_t i=n; i>0; --i) for (std::size_t j=m; j>0; --j) {
      cell(i-1,j-1) = old[prefix+i-1] == fresh[prefix+j-1] ? cell(i,j)+1 : std::max(cell(i,j-1),cell(i-1,j));
    }
    std::size_t i=0, j=0;
    while (i<n || j<m) {
      if (i<n && j<m && old[prefix+i] == fresh[prefix+j]) { emit(' ',old[prefix+i]); ++i; ++j; }
      else if (i<n && (j==m || cell(i+1,j)>=cell(i,j+1))) emit('-',old[prefix+i++]);
      else emit('+',fresh[prefix+j++]);
    }
  }
  for (std::size_t i=old.size()-suffix; i<old.size(); ++i) emit(' ',old[i]);
  return output.str();
}
}  // namespace kc::vault
