# Search library

The search library implements MVP step 3's local, concept-first lookup over
generated articles. `VaultWriter` remains responsible for updating
`article_fts` only after a successful atomic article write; `SearchIndex` is a
read-only service over that committed state.

Search behavior:

- searches the FTS5 `title`, `aliases`, and `body` columns;
- requires every normalized query term and treats user punctuation as a
  separator rather than executable FTS syntax;
- weights title matches above aliases and aliases above body matches;
- orders equal scores deterministically by title and article ID;
- returns article ID, title, a short highlighted excerpt, and the
  project-relative vault path; and
- accepts 1–100 results, defaulting to 20.

The CLI exposes the service through:

```bash
kc search "markov chains"
kc search "transition probability" --limit 5 --json
```

An empty index is a successful search with no results. Queries without a word
or number are rejected as invalid input, and stored absolute or parent-escaping
vault paths are rejected as corrupt state.
