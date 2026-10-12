-- Content hashes describe bytes, not logical identity. Preserve every version
-- ID and row order (the latest selector uses rowid), including all historical
-- page/citation references, while removing global hash uniqueness.
CREATE TABLE source_versions_rebuilt (
  source_version_id TEXT PRIMARY KEY,
  source_id TEXT NOT NULL REFERENCES sources(source_id),
  sha256 TEXT NOT NULL,
  original_filename TEXT NOT NULL,
  stored_path TEXT NOT NULL,
  media_type TEXT NOT NULL,
  byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
  imported_at TEXT NOT NULL
);

INSERT INTO source_versions_rebuilt
  (rowid, source_version_id, source_id, sha256, original_filename, stored_path,
   media_type, byte_size, imported_at)
SELECT rowid, source_version_id, source_id, sha256, original_filename, stored_path,
       media_type, byte_size, imported_at
FROM source_versions ORDER BY rowid;

DROP TABLE source_versions;
ALTER TABLE source_versions_rebuilt RENAME TO source_versions;
CREATE INDEX source_versions_hash ON source_versions(sha256);
CREATE INDEX source_versions_source ON source_versions(source_id);
