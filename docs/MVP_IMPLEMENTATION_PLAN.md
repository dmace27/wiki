# MVP Implementation Plan

## Outcome

The MVP is complete when a user can import real probability notes, inspect or
correct their page extraction, compile a cited `Markov Chains.md` proposal with
a local model, approve it, write it safely to an Obsidian vault, and find it
with local keyword search.

## Work sequence

### 0. Foundation and contracts

**Owner:** technical lead

Create the C++/CMake skeleton, vcpkg manifest, test runner, migration runner,
and the domain types defined in the documentation set.

**Acceptance criteria**

- `kc init` creates a valid project without requiring a model or OCR runtime.
- The SQLite migration succeeds on a fresh project.
- All JSON fixtures validate against the documented schemas.
- The build enables warnings and sanitizer-enabled test presets.

### 1A. Source import and immutable state

**Owner:** import/state agent

Implement Markdown, TXT, and PDF registration, SHA-256 hashing, deduplication,
and source-version storage.

**Acceptance criteria**

- An unchanged source re-import is idempotent.
- Changed content creates a new `source_version` rather than modifying history.
- The project retains an immutable copy of every imported file; source links
  remain valid even if the original file is moved or deleted.
- No absolute path is persisted in the project database.

### 1B. Page extraction

**Owner:** PDF/OCR agent

Implement direct text extraction for text PDFs, page rendering for PDFs, and a
pluggable local OCR adapter for pages without usable native text.

**Acceptance criteria**

- Every PDF page produces a `source_pages` row with a page number.
- Rendered page images are retained using project-relative paths.
- Markdown/TXT imports create one logical page.
- Failed OCR is represented as `failed`; it is never silently treated as good
  text.

### 1C. Local model and structured output

**Owner:** model agent

Implement an Ollama-compatible local `LanguageModel` adapter and a
schema-constrained Markov Chains prompt.

**Acceptance criteria**

- The adapter reads model settings from `kc.json`.
- It records provider, model, prompt version, hashes, status, and a redacted
  diagnostic record.
- Invalid JSON or invalid citations yield `invalid_response` and no proposal.
- No provider implementation can write vault files.

### 2A. Compiler and proposal validation

**Owner:** compiler agent

Select Markov-chain-relevant pages with deterministic title, alias, and keyword
matching. Call the model, validate its proposal, and store the proposal and
normalized citations.

**Acceptance criteria**

- `kc compile --concept "Markov Chains"` creates a pending proposal.
- Every generated block has at least one valid citation.
- Citation offsets and normalized quotes match extracted text.
- Invalid proposals are stored only as failed model runs, not pending proposals.

### 2B. Safe Markdown writer

**Owner:** vault agent

Render an approved proposal into the documented Obsidian Markdown format and
atomically apply it.

**Acceptance criteria**

- Citation footnotes point to a copied or linked source PDF at the cited page.
- Existing user-authored text outside managed markers remains byte-for-byte
  unchanged.
- The old file is backed up before write and each application is recorded.
- Search indexing occurs after a successful write only.

### 2C. Extraction and proposal review

**Owner:** review-surface agent

Implement a minimal review experience. It may begin as terminal output plus a
local browser page, but it must make source evidence inspectable.

**Acceptance criteria**

- A reviewer can see page image, extracted text, and extraction status together.
- A reviewer can correct page text before compilation.
- A reviewer can inspect proposal sections and their citations before approving.
- The review surface does not edit the article itself.

### 3. Search and end-to-end verification

**Owner:** search/QA agent

Implement SQLite FTS search and a full integration test path using permitted
fixtures plus manual testing on actual probability notes.

**Acceptance criteria**

- `kc search "markov chains"` returns the article title and vault path.
- A test covers import, extraction, proposal creation, approval, apply, and
  search.
- Manual results document OCR accuracy and source-link behavior in Obsidian.

### 4. Five-concept expansion

Only after the Markov Chains workflow is useful, generalize it to Conditional
Probability, Bayes' Rule, Random Variables, and Random Walks.

**Acceptance criteria**

- Existing article titles are reused instead of creating duplicates.
- New proposals remain source-linked and approval-gated.
- Topic maps, embeddings, and automatic relationship inference remain deferred.

## Dependency graph

```text
Foundation
  ├── Import/state ──┐
  ├── Page extraction├── Compiler/proposals ── Vault writer ── Search/QA
  └── Local model ───┘                │
                                      └── Review surface
```

Agents may work in parallel only after their input/output contract is stable.
No agent should change another agent's public data contract without updating the
documentation and migration/version plan.
