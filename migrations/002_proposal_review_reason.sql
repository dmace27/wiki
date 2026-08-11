-- Preserve an optional human explanation for proposal rejection. The proposal
-- payload remains immutable; this column belongs only to review metadata.
ALTER TABLE proposals ADD COLUMN review_reason TEXT;
