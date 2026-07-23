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
  source_version_id TEXT NOT NULL REFERENCES source_versions(source_version_id),
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
  source_version_id TEXT NOT NULL REFERENCES source_versions(source_version_id),
  extractor_name TEXT NOT NULL,
  extractor_version TEXT,
  ocr_provider TEXT,
  status TEXT NOT NULL CHECK (status IN ('running', 'completed', 'failed')),
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
  status TEXT NOT NULL CHECK (status IN ('completed', 'invalid_response', 'failed')),
  started_at TEXT NOT NULL,
  completed_at TEXT,
  error_message TEXT
);

CREATE TABLE proposals (
  proposal_id TEXT PRIMARY KEY,
  article_id TEXT REFERENCES articles(article_id),
  model_run_id TEXT REFERENCES model_runs(run_id),
  operation TEXT NOT NULL CHECK (operation IN ('create_article', 'update_article')),
  payload_json TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'rejected', 'applied', 'superseded')),
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

