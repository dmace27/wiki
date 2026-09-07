# Knowledge Compiler MVP

Build one trustworthy end-to-end system that turns uploaded learning material
from **any subject** into searchable, source-grounded concept articles. Markov
chains is a useful test case, not a product boundary.

The MVP works one concept at a time so every result stays reviewable:

1. Import real PDF, scanned-PDF, Markdown, and text notes from any domain.
2. Extract text locally while retaining document versions, page numbers, and
   page images.
3. Ask for an article by concept title, such as “Markov Chains,” “Cellular
   Respiration,” “The French Revolution,” or “Color Theory.”
4. Select relevant pages using the concept title and its meaningful terms. If
   the concept is described implicitly, allow the user to nominate source files
   as curated evidence.
5. Use a local model to draft a schema-validated article adapted to the subject.
6. Link every generated section or claim to exact spans in the uploaded notes.
7. Review and approve the proposal before writing or updating an Obsidian page.
8. Search across all approved concept articles by title, alias, and content.

Success means a user can upload mixed learning material, compile several
unrelated concepts, read useful articles in language close to their own notes,
and jump back to the exact pages where they learned each idea. Creating a
proposal for one new concept must never overwrite or supersede work for another.

## Article contract

The structure is intentionally domain-neutral. The model adapts the contents of
each section to the evidence instead of assuming a mathematical topic.

```markdown
# <Concept title>

## My working explanation
...

## Key ideas
- ...

## Example
...

## Related concepts
- [[Another concept]]

## Sources
- Source name, page N
```

Every generated block and related-concept link requires at least one verified
citation. A section may be omitted when the uploaded evidence does not support
it; the model must not fill gaps with outside knowledge.

## Product boundaries

“Any subject” means the concept pipeline contains no hard-coded domain,
vocabulary, aliases, article title, or output filename. It does not mean every
possible file format or perfect automatic concept discovery in the first
release.

The MVP supports:

- arbitrary safe concept titles and deterministic concept slugs;
- subject-neutral prompts and validation;
- deterministic title/term evidence matching across all active sources;
- explicit source curation for implicit, symbolic, niche, or non-English topics;
- independent create/update proposals for multiple concepts;
- local extraction, model execution, review, vault writing, and search.

Defer embeddings, automatic ontology construction, bulk concept discovery,
daemonized ingestion, a full graph editor, cloud APIs, and a custom web UI until
the generic one-concept loop works well on several genuinely different subjects.

## Build order

1. **Local import and extraction** — support Markdown, text, and representative
   PDFs from several domains.
2. **Extraction review** — show extracted text beside its source page and allow
   corrections before model use.
3. **Generic local-model prompt** — generate structured content for the exact
   requested concept using only selected evidence.
4. **Generic concept selection** — match the requested title and meaningful
   terms; accept explicitly curated sources when text does not name the concept.
5. **Review and Markdown writer** — validate citations, show the proposed change,
   require approval, and safely create or update `<Concept title>.md`.
6. **Concept-first search** — search every applied article by title, aliases, and
   body text.
7. **Cross-domain QA** — prove the complete workflow with at least three
   unrelated concepts and both automatic and curated evidence selection.

## Technical stack

- C++23 and CMake for the compiler and CLI
- SQLite for durable state and local full-text search
- Poppler plus pluggable local OCR for PDFs and scans
- a provider-neutral language-model interface with Ollama/Gemma as the first
  local adapter
- Obsidian-compatible Markdown output
- a later local web review UI

| Order | Deliverable | Done when |
| --- | --- | --- |
| 0 | Generic contracts and scaffold | Schemas, database tables, CLI commands, safe concept identity rules, and local-first configuration contain no subject-specific restriction. |
| 1A | Import and immutable state | Markdown, text, and PDF sources are hashed, copied locally, versioned, and idempotent. |
| 1B | Per-page extraction | Native PDF text is preferred, page images are retained, OCR is pluggable, and each page has an explicit review status. |
| 1C | Generic model adapter | Given a requested title and selected pages, the local model returns only a schema-valid proposal with exact-span citations. |
| 2A | Concept compiler | Arbitrary concepts use deterministic title/term matching or explicit source curation; existing articles are detected by requested title. |
| 2B | Safe vault writer | An approved proposal creates or updates `<Concept title>.md`, preserves unmanaged content, links sources, and never silently overwrites user files. |
| 2C | Review experience | Users can correct extraction, inspect proposal diffs and evidence, and approve or reject without implicit vault writes. |
| 3A | Search | All applied concepts are searchable locally by title, aliases, and article body. |
| 3B | End-to-end QA | Import → extract → review → compile → approve → apply → search passes for unrelated topics such as probability, biology, and history. |
| 4 | Later expansion | Add bulk concept suggestions, embeddings, topic maps, and automatic relationship inference without weakening provenance or review. |
