# Compiler library

The compiler library implements MVP step 2A. It is the application service
between extracted page state and the approval-gated proposal tables; it never
writes vault files.

For any requested concept, evidence selection:

- reads only active sources and each source's latest immutable version;
- excludes failed, empty, and unreviewed OCR pages;
- optionally restricts selection to explicit `--source` IDs;
- ranks exact title matches above matches containing every meaningful title
  term using Unicode NFC/case folding, with a small ASCII singular/plural fallback;
- treats explicitly filtered sources as curated evidence when no lexical match
  exists; and
- orders matches by a stable relevance score, normalized source name, source
  ID, page number, and page ID.

`Compiler::compile` sends that ordered evidence to `ModelRunner`, which strictly
parses and validates the model response. Valid citation quotes are canonicalized
from their exact UTF-8 byte spans by trimming and collapsing whitespace. The
compiler then creates the immutable pending proposal and all normalized
`proposal_citations` plus all selected `proposal_evidence` in one SQLite
transaction. Page revisions, exact bytes/status, current source versions, and
retained source/image hashes are rechecked under the publication write lock.
Changed evidence rejects publication before any pending work is superseded. A newer proposal supersedes
older pending or approved work for the same article.

Invalid JSON, missing block citations, foreign page IDs, invalid byte offsets,
and quote mismatches remain `invalid_response` model runs. None can create a
proposal, and compilation never changes the vault.

Concept lookup, pending supersession, and collision checks share the
[concept identity contract](../../docs/CONCEPT_IDENTITY.md). The compiler resolves
stored display spelling/slugs before calling the model and rechecks identity
and destination ownership under the proposal publication transaction.

See [extraction integrity](../../docs/EXTRACTION_INTEGRITY.md) for evidence
freezing, supported concurrency, and reproduction.

## Accumulated updates

Updates deterministically retain approved blocks, relationships, and historical
citations before adding new source-grounded content. `--source` filters new
model evidence; `--regenerate` deliberately proposes complete replacement and
review lists all removed and added claims. A compiler-owned base apply-run ID
and managed-region digest guard publication, approval, and apply. Manual notes
belong outside the markers; managed edits require restoration and fresh review.
See [update semantics, revision checks, and legacy recovery](../../docs/ACCUMULATED_KNOWLEDGE.md)
for the exact reconciliation and review JSON contracts.
