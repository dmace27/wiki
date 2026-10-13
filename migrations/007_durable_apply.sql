-- A file journal is durable before vault changes. This commit marker is inserted
-- in the SAME transaction as article/citation/audit/FTS publication. Recovery
-- rolls back files when the marker is absent, and cleans up when it is present.
CREATE TABLE file_operations (
  operation_id TEXT PRIMARY KEY,
  operation_kind TEXT NOT NULL CHECK (operation_kind IN ('apply','restore')),
  article_id TEXT NOT NULL REFERENCES articles(article_id),
  completed_at TEXT NOT NULL
);

-- Complete post-apply bytes allow exact restoration without regenerating text.
-- Older audit rows remain readable but have no fabricated historical snapshot.
CREATE TABLE article_snapshots (
  apply_run_id TEXT PRIMARY KEY REFERENCES apply_runs(apply_run_id),
  content TEXT NOT NULL,
  aliases TEXT NOT NULL,
  vault_config TEXT NOT NULL
);
ALTER TABLE articles ADD COLUMN current_apply_run_id TEXT REFERENCES apply_runs(apply_run_id);
UPDATE articles SET current_apply_run_id = (
  SELECT apply_run_id FROM apply_runs WHERE apply_runs.article_id = articles.article_id
  ORDER BY rowid DESC LIMIT 1
);
CREATE TABLE restore_runs (
  restore_run_id TEXT PRIMARY KEY REFERENCES file_operations(operation_id),
  article_id TEXT NOT NULL REFERENCES articles(article_id),
  from_apply_run_id TEXT REFERENCES apply_runs(apply_run_id),
  to_apply_run_id TEXT NOT NULL REFERENCES apply_runs(apply_run_id),
  previous_content_sha256 TEXT,
  restored_content_sha256 TEXT NOT NULL,
  backup_path TEXT,
  restored_at TEXT NOT NULL
);
