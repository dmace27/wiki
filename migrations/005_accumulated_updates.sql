-- Fingerprint only compiler-owned bytes so surrounding user edits remain safe.
ALTER TABLE apply_runs ADD COLUMN managed_sha256 TEXT;

-- Update semantics and the exact approved base are persisted independently of
-- model output. Older proposals cannot invent a safe base during approval.
CREATE TABLE proposal_update_bases (
  proposal_id TEXT PRIMARY KEY REFERENCES proposals(proposal_id),
  base_apply_run_id TEXT NOT NULL REFERENCES apply_runs(apply_run_id),
  managed_sha256 TEXT NOT NULL,
  update_mode TEXT NOT NULL CHECK (update_mode IN ('additive', 'regenerate'))
);
