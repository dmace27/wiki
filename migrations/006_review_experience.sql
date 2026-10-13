-- Preserve every historical identity and byte offset while adding a current
-- extraction pointer. Only current rows must be unique per source/page number.
CREATE TABLE source_pages_rebuilt (
  page_id TEXT PRIMARY KEY,
  source_version_id TEXT NOT NULL REFERENCES source_versions(source_version_id),
  page_number INTEGER NOT NULL CHECK (page_number >= 1),
  image_path TEXT,
  text TEXT NOT NULL DEFAULT '',
  text_status TEXT NOT NULL CHECK (text_status IN ('native','ocr_unreviewed','reviewed','failed')),
  text_sha256 TEXT NOT NULL,
  evidence_revision INTEGER NOT NULL DEFAULT 0 CHECK (evidence_revision >= 0),
  image_sha256 TEXT,
  is_current INTEGER NOT NULL DEFAULT 1 CHECK (is_current IN (0,1)),
  supersedes_page_id TEXT REFERENCES source_pages(page_id)
);
INSERT INTO source_pages_rebuilt
  (rowid,page_id,source_version_id,page_number,image_path,text,text_status,text_sha256,evidence_revision,image_sha256)
SELECT rowid,page_id,source_version_id,page_number,image_path,text,text_status,text_sha256,evidence_revision,image_sha256
FROM source_pages ORDER BY rowid;
DROP TABLE source_pages;
ALTER TABLE source_pages_rebuilt RENAME TO source_pages;
CREATE UNIQUE INDEX current_source_page ON source_pages(source_version_id,page_number) WHERE is_current = 1;
CREATE INDEX source_page_history ON source_pages(source_version_id,page_number);

-- Approval binds the reviewed rendering and collision consent independently
-- of the model's proposal. No article file is written during this transition.
CREATE TABLE proposal_approvals (
  proposal_id TEXT PRIMARY KEY REFERENCES proposals(proposal_id),
  preview_sha256 TEXT NOT NULL,
  allow_overwrite_user_file INTEGER NOT NULL CHECK (allow_overwrite_user_file IN (0,1))
);
