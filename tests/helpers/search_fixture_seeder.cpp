#include "kc/storage/database.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

/// Seed one applied article for the black-box CLI search test.
///
/// The full workflow integration test creates this state through production
/// services. This focused helper avoids depending on an external `sqlite3`
/// executable while letting CTest invoke the real `kc search` process against
/// a populated FTS5 index on every supported platform.
int main(const int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: kc_search_fixture_seeder PROJECT_ROOT\n";
    return 2;
  }

  try {
    const auto state_path = std::filesystem::path(argv[1]) /
                            ".knowledge-compiler/state.sqlite";
    kc::storage::Database database(state_path);
    database.execute(
        "INSERT INTO articles(article_id, title, slug, vault_path, created_at, "
        "updated_at) VALUES ('art_01J00000000000000000000000', "
        "'Markov Chains', 'markov-chains', 'vault/Markov Chains.md', "
        "'2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');");
    database.execute(
        "INSERT INTO article_fts(article_id, title, aliases, body) VALUES "
        "('art_01J00000000000000000000000', 'Markov Chains', "
        "'Markov chain', 'A Markov chain moves between states.');");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "could not seed search fixture: " << error.what() << '\n';
    return 1;
  }
}
