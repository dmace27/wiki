-- Revisions detect corrections/re-extractions including text A -> B -> A.
ALTER TABLE source_pages ADD COLUMN evidence_revision INTEGER NOT NULL DEFAULT 0
  CHECK (evidence_revision >= 0);
-- NULL on legacy PDFs means the old image was never integrity-verified.
ALTER TABLE source_pages ADD COLUMN image_sha256 TEXT;

-- Freeze every page supplied to a successful generation, including context
-- that did not receive an explicit citation in the model's output.
CREATE TABLE proposal_evidence (
  proposal_id TEXT NOT NULL REFERENCES proposals(proposal_id),
  page_id TEXT NOT NULL REFERENCES source_pages(page_id),
  evidence_revision INTEGER NOT NULL,
  text_sha256 TEXT NOT NULL,
  image_sha256 TEXT,
  source_sha256 TEXT NOT NULL,
  PRIMARY KEY (proposal_id, page_id)
);
