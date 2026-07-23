Make one narrow end-to-end prototype: **compile your probability notes into a searchable Markov Chains article.**

Call it a “Personal Probability Compiler.” Its only job is:

1. Import a few of your real Goodnotes PDFs and Markdown notes.
2. Extract their text locally, retaining page numbers and images.
3. Find notes related to Markov chains.
4. Use a local model to draft one `Markov Chains.md` article from those notes.
5. Link every section or claim back to the original note page.
6. Open the article in Obsidian.

Success means you can search “Markov chains,” read a useful article in language close to your own notes, and jump back to the pages where you learned it.

A good first article structure is:

```
# Markov Chains

## My working explanation
...

## Key ideas
- State
- Transition probability
- Markov property
- Stationary distribution

## Example
...

## Related concepts
- [[Conditional Probability]]
- [[Random Variables]]
- [[Random Walks]]

## Sources
- Probability Notes, Week 8, page 3
- Probability Notes, Week 9, page 1
```

Build the prototype in this order:

1. **Local import and extraction** — support Markdown plus a small set of your actual PDFs.
2. **Extraction review** — show the text beside each source page, because handwriting/OCR quality determines everything downstream.
3. **One local-model prompt** — turn selected extracted pages into schema-validated content for one article.
4. **Markdown writer** — create the page in an Obsidian vault, preserving source links.
5. **Simple search** — search article titles and article text; semantic search can come later.
6. **Expand to five concepts** — for example Markov Chains, Conditional Probability, Bayes’ Rule, Random Variables, and Random Walks.


Tech Stack:
C++23 + CMake compiler/CLI, 
SQLite, 
Poppler,
pluggable OCR, 
Ollama/Gemma adapter, 
and a later local web review UI. 


| Order | Agent task | Deliverable / done when |
|---|---|---|
| 0 | **Technical lead: define the MVP contract and scaffold** | Select the stack, create project structure, define source/extraction/article JSON schemas, SQLite tables, citation-link format, CLI commands, and a sample project configuration. Local processing must be the default; API provider remains an interface only. |
| 1A | **Import/state agent** | `import` command for Markdown, TXT, and PDF files; immutable source IDs and content hashes; source metadata and original-file registration/copying recorded in SQLite. Re-importing an unchanged file is idempotent. |
| 1B | **PDF extraction agent** | Per-page extraction pipeline: direct PDF text first, rendered page images retained, and a pluggable local OCR adapter for scanned pages. Output is normalized page records with source ID, page number, text, image path, and extraction status. |
| 1C | **Local-model agent** | A local LLM provider adapter (for example, Ollama + Gemma) and a strict structured-output prompt. Given selected page records, it returns schema-validated proposed content for exactly one concept: Markov Chains. No arbitrary Markdown generation. |
| 2A | **Concept-selection/compiler agent** | Finds Markov-chain-relevant extracted pages using deterministic keyword/alias matching initially, invokes the local model, validates citations against actual page records, and writes a reviewable proposed change set. |
| 2B | **Vault writer agent** | Converts an approved proposal into `vault/Markov Chains.md`, preserving the required sections, `[[Concept Links]]`, and source links such as `[_sources/probability-notes.pdf#page=3]`. Must never overwrite a user-authored file without an explicit approval flag. |
| 2C | **Review experience agent** | A minimal local review surface: show each PDF page image beside extracted text, allow text correction, display the proposed article/diff and cited source pages, then approve or reject the proposal. This can be a lightweight local web UI—do not build a full editor. |
| 3A | **Search agent** | Local concept-first search over generated article titles and body text. Searching “markov chains” should return the generated article and open its Markdown file / provide its Obsidian path. |
| 3B | **End-to-end QA agent** | Add fixtures using a Markdown note and a small, permitted PDF; test import → extraction → review → compile → approval → vault write → search. Manually test against your actual Goodnotes export and record OCR limitations. |
| 4 | **Expansion agent** | Generalize only after the full loop works: Conditional Probability, Bayes’ Rule, Random Variables, and Random Walks. Add incremental updates to existing concept pages and basic duplicate-title protection. |


