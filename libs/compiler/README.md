# Compiler library

The compiler library implements MVP step 2A. It is the application service
between extracted page state and the approval-gated proposal tables; it never
writes vault files.

For `Markov Chains`, evidence selection:

- reads only active sources and each source's latest immutable version;
- excludes failed, empty, and unreviewed OCR pages;
- optionally restricts selection to explicit `--source` IDs;
- matches the title (`Markov Chains`), alias (`Markov chain`), and a fixed set
  of topic-specific keywords; and
- orders matches by a stable relevance score, normalized source name, source
  ID, page number, and page ID.

`Compiler::compile` sends that ordered evidence to `ModelRunner`, which strictly
parses and validates the model response. Valid citation quotes are canonicalized
from their exact UTF-8 byte spans by trimming and collapsing whitespace. The
compiler then creates the immutable pending proposal and all normalized
`proposal_citations` in one SQLite transaction. A newer proposal supersedes
older pending or approved work for the same article.

Invalid JSON, missing block citations, foreign page IDs, invalid byte offsets,
and quote mismatches remain `invalid_response` model runs. None can create a
proposal, and compilation never changes the vault.
