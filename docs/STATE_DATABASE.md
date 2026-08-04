# State Database

The project state database is `.knowledge-compiler/state.sqlite`. Markdown is
the portable, user-facing artifact; SQLite stores provenance, cache state,
review state, audit history, and the local keyword index.

## Database rules

- Enable `PRAGMA foreign_keys = ON` for every connection.
- Enable WAL mode during database initialization.
- Run each migration inside a transaction.
- Do not alter an applied migration; create the next numbered migration.
- Store file paths relative to the project root.
- Keep page images and source files on disk; store their relative paths in
  SQLite rather than BLOBs.
- Do not store credentials or unredacted request headers.

## Tables

| Table | Purpose |
| --- | --- |
| `schema_migrations` | Applied migration versions. |
| `sources` | Logical source identity and display metadata. |
| `source_versions` | Immutable, content-hashed imported versions. |
| `source_pages` | Page-level text, image reference, and review status. |
| `extraction_runs` | Extraction/OCR diagnostics. |
| `model_runs` | Model request metadata and validated/raw response record. |
| `articles` | Generated concept article identity and vault location. |
| `proposals` | Pending/reviewed/applied structured changes. |
| `proposal_citations` | Evidence attached to proposed generated blocks. |
| `article_citations` | Evidence currently represented in an applied article. |
| `apply_runs` | Vault-write audit and backup reference. |
| `article_fts` | FTS5 search index over generated articles. |

## Initial migration

Save this as `migrations/001_initial.sql` when implementation begins.

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

CREATE TABLE schema_migrations (
  version INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
);

CREATE TABLE sources (
  source_id TEXT PRIMARY KEY,
  display_name TEXT NOT NULL,
  source_kind TEXT NOT NULL
    CHECK (source_kind IN ('pdf', 'markdown', 'text')),
  created_at TEXT NOT NULL,
  archived_at TEXT
);

CREATE TABLE source_versions (
  source_version_id TEXT PRIMARY KEY,
  source_id TEXT NOT NULL REFERENCES sources(source_id),
  sha256 TEXT NOT NULL UNIQUE,
  original_filename TEXT NOT NULL,
  stored_path TEXT NOT NULL,
  media_type TEXT NOT NULL,
  byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
  imported_at TEXT NOT NULL
);

CREATE TABLE source_pages (
  page_id TEXT PRIMARY KEY,
  source_version_id TEXT NOT NULL
    REFERENCES source_versions(source_version_id),
  page_number INTEGER NOT NULL CHECK (page_number >= 1),
  image_path TEXT,
  text TEXT NOT NULL DEFAULT '',
  text_status TEXT NOT NULL
    CHECK (text_status IN ('native', 'ocr_unreviewed', 'reviewed', 'failed')),
  text_sha256 TEXT NOT NULL,
  UNIQUE (source_version_id, page_number)
);

CREATE TABLE extraction_runs (
  run_id TEXT PRIMARY KEY,
  source_version_id TEXT NOT NULL
    REFERENCES source_versions(source_version_id),
  extractor_name TEXT NOT NULL,
  extractor_version TEXT,
  ocr_provider TEXT,
  status TEXT NOT NULL
    CHECK (status IN ('running', 'completed', 'failed')),
  started_at TEXT NOT NULL,
  completed_at TEXT,
  error_message TEXT
);

CREATE TABLE articles (
  article_id TEXT PRIMARY KEY,
  title TEXT NOT NULL COLLATE NOCASE UNIQUE,
  slug TEXT NOT NULL UNIQUE,
  vault_path TEXT NOT NULL UNIQUE,
  content_sha256 TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE model_runs (
  run_id TEXT PRIMARY KEY,
  provider TEXT NOT NULL,
  model TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  request_sha256 TEXT NOT NULL,
  response_json TEXT,
  status TEXT NOT NULL
    CHECK (status IN ('completed', 'invalid_response', 'failed')),
  started_at TEXT NOT NULL,
  completed_at TEXT,
  error_message TEXT
);

CREATE TABLE proposals (
  proposal_id TEXT PRIMARY KEY,
  article_id TEXT REFERENCES articles(article_id),
  model_run_id TEXT REFERENCES model_runs(run_id),
  operation TEXT NOT NULL
    CHECK (operation IN ('create_article', 'update_article')),
  payload_json TEXT NOT NULL,
  status TEXT NOT NULL
    CHECK (status IN ('pending', 'approved', 'rejected', 'applied', 'superseded')),
  created_at TEXT NOT NULL,
  reviewed_at TEXT,
  applied_at TEXT
);

CREATE TABLE proposal_citations (
  proposal_id TEXT NOT NULL REFERENCES proposals(proposal_id),
  section_key TEXT NOT NULL,
  block_index INTEGER NOT NULL CHECK (block_index >= 0),
  page_id TEXT NOT NULL REFERENCES source_pages(page_id),
  start_char INTEGER NOT NULL CHECK (start_char >= 0),
  end_char INTEGER NOT NULL CHECK (end_char > start_char),
  quote TEXT NOT NULL,
  PRIMARY KEY (proposal_id, section_key, block_index, page_id, start_char)
);

CREATE TABLE article_citations (
  article_id TEXT NOT NULL REFERENCES articles(article_id),
  section_key TEXT NOT NULL,
  block_index INTEGER NOT NULL,
  page_id TEXT NOT NULL REFERENCES source_pages(page_id),
  PRIMARY KEY (article_id, section_key, block_index, page_id)
);

CREATE TABLE apply_runs (
  apply_run_id TEXT PRIMARY KEY,
  proposal_id TEXT NOT NULL UNIQUE REFERENCES proposals(proposal_id),
  article_id TEXT NOT NULL REFERENCES articles(article_id),
  previous_content_sha256 TEXT,
  new_content_sha256 TEXT NOT NULL,
  backup_path TEXT,
  applied_at TEXT NOT NULL
);

CREATE VIRTUAL TABLE article_fts USING fts5(
  article_id UNINDEXED,
  title,
  aliases,
  body,
  tokenize = 'unicode61 remove_diacritics 2'
);
```

## Indexing and lifecycle

- Update `article_fts` only after a successful atomic Markdown write.
- A proposal is immutable after creation. Create a new proposal rather than
  editing its payload.
- `proposal_citations` are created transactionally with the proposal.
- `article_citations` are replaced transactionally with a successful apply.
- An imported source is never overwritten. New content means a new
  `source_versions` row and new page rows.
- A completed model run stores only its strictly validated proposal JSON.
  Invalid model output is replaced by a redacted SHA-256/size diagnostic;
  transport error bodies, prompts, evidence text, URLs, and headers are not
  persisted in `model_runs`.
- Step 1C writes `model_runs` only. A pending `proposals` row is created later
  and transactionally by the compiler after all compiler-level checks pass.
