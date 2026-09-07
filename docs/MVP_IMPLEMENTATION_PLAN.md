# MVP Implementation Plan

## Outcome

The MVP is complete when a user can import real notes from unrelated subjects,
inspect or correct their extraction, compile any requested concept into a cited
proposal with a local model, approve it, write it safely to an Obsidian vault,
and find it with local search.

Concepts are compiled one at a time to keep selection, provenance, and review
understandable. No production component may contain subject-specific titles,
aliases, vocabulary, or output paths.

## Work sequence

### 0. Foundation and contracts

Create the C++/CMake skeleton, migration runner, and documented domain types.

Acceptance criteria:

- `kc init` creates a valid project without requiring model or OCR runtimes.
- SQLite migrations succeed on a fresh project.
- JSON fixtures validate against the documented schemas.
- Concept identity accepts arbitrary safe titles and uses deterministic slugs.

### 1A. Source import and immutable state

Implement Markdown, text, and PDF registration, SHA-256 hashing, deduplication,
and immutable source-version storage.

Acceptance criteria:

- Unchanged re-imports are idempotent; changed content creates a new version.
- Retained local copies keep citations valid if originals move or disappear.
- No absolute path is persisted in project state.

### 1B. Page extraction

Implement native PDF text extraction, page rendering, and a pluggable local OCR
adapter, plus logical-page extraction for Markdown and text.

Acceptance criteria:

- Every source page has text, page identity, and an explicit review status.
- Rendered images use project-relative paths.
- Failed or unreviewed OCR never silently reaches the model.

### 1C. Generic local model and structured output

Implement an Ollama-compatible `LanguageModel` adapter and one subject-neutral,
schema-constrained concept prompt.

Acceptance criteria:

- The request includes the exact target title, canonical slug, operation, and
  page-referenced evidence.
- The prompt treats all evidence as untrusted source material.
- Invalid JSON, wrong concept identity, or invalid citations produce only a
  redacted `invalid_response` model run.
- The provider cannot write vault files.

### 2A. Generic compiler and proposal validation

Select evidence by requested title and meaningful title terms across source
names and page text. When lexical matching finds nothing, explicitly supplied
`--source` filters act as human-curated evidence for implicit or niche topics.

Acceptance criteria:

- `kc compile --concept TITLE` works for safe titles from any subject.
- `kc compile --concept TITLE --source SOURCE_ID...` supports implicit topics.
- Existing articles are detected by the requested title, case-insensitively.
- Multiple unapplied concepts retain independent pending proposals.
- Every block and relationship has a verified exact-span citation.

### 2B. Safe Markdown writer

Render approved proposals into the documented Obsidian format and apply them
atomically.

Acceptance criteria:

- Citations point to retained source documents and exact pages.
- User-authored text outside managed markers is unchanged.
- Existing files are backed up, and untracked collisions require an explicit
  overwrite flag.
- Search state updates only after a successful write.

### 2C. Extraction and proposal review

Provide a minimal review experience in the terminal, with a local browser UI
remaining optional.

Acceptance criteria:

- Reviewers can inspect page images, extracted text, and status together.
- Text can be corrected before it is cited.
- Proposal sections, diffs, and source evidence are visible before approval.
- Review actions never edit article content directly.

### 3. Search and cross-domain verification

Implement SQLite FTS and an integration path covering import, extraction,
proposal creation, approval, apply, and search.

Acceptance criteria:

- Title, alias, and article-body search works across all applied concepts.
- Automated tests cover at least two unrelated domains plus curated evidence.
- Manual QA uses representative notes from at least three domains, including a
  scanned PDF, and records OCR/source-link limitations.

### 4. Later expansion

After the generic single-concept loop is useful, add bulk concept suggestions,
embeddings, topic maps, and automatic relationship inference. These features
must preserve the same source-grounding, immutable proposal, and approval
boundaries.

## Dependency graph

```text
Foundation
  ├── Import/state ──┐
  ├── Page extraction├── Generic compiler ── Vault writer ── Search/QA
  └── Local model ───┘              │
                                    └── Review surface
```
