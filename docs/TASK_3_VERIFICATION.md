# Task 3 Verification Record

This record separates repeatable automated verification from environment- and
user-data-dependent manual checks. It must not be read as an OCR quality claim
for notes that were not available during implementation.

## Automated end-to-end result

**Date:** 2026-08-11  
**Environment:** macOS, AppleClang 21, SQLite 3 with FTS5

The `import through apply produces a searchable source-linked article`
integration test completes the following real service path:

1. initialize a project and apply migrations;
2. import a project-owned synthetic PDF fixture into immutable source storage;
3. render/extract one page through injected local PDF/OCR adapter boundaries;
4. inspect and correct the page through `ReviewService`;
5. create a schema- and citation-validated proposal through `Compiler` and a
   deterministic local `LanguageModel` double;
6. approve without writing or indexing;
7. atomically apply through `VaultWriter`;
8. confirm the immutable PDF was copied into the vault and the generated
   footnote contains `#page=1`;
9. search `markov chains` through SQLite FTS5; and
10. receive `Markov Chains` and `vault/Markov Chains.md`.

The complete development suite passed: 49/49 CTest tests. The end-to-end model
double is deterministic and offline, but all parsing, validation, database
transactions, Markdown rendering, source copying, FTS indexing, and searching
are production implementations.

## OCR accuracy result

An accuracy percentage for actual handwritten probability notes was **not
measured** in this workspace. No user Goodnotes export or other real
probability-note PDF is checked into the repository, and the current execution
environment does not provide `tesseract` or `pdftotext`. Reporting a synthetic
adapter result as handwriting OCR accuracy would be misleading.

The automated PDF tests do establish the failure contract needed for later
manual sampling:

- every rendered PDF page gets a row and project-relative retained image;
- usable native text bypasses OCR;
- successful OCR remains `ocr_unreviewed` until reviewed; and
- missing, failed, or unusable OCR is stored as `failed`, never as good text.

When real notes and Tesseract are available, record accuracy page by page using
the following reproducible sample rather than a subjective summary:

| Source/page | Native/OCR | Characters expected | Character errors | Accuracy | Main error types |
| --- | --- | ---: | ---: | ---: | --- |
| _Not run—fixture unavailable_ | — | — | — | — | — |

Use `kc review extraction SOURCE_ID --page N` to compare the retained image and
text, then calculate `(expected characters - substitutions - insertions -
deletions) / expected characters`. Record formulas, arrows, subscripts, and
handwriting separately because aggregate prose accuracy can hide the most
important probability-notation failures.

## Obsidian source-link result

The generated artifact was verified mechanically:

- the cited immutable PDF exists beneath the configured vault `_sources`
  directory;
- its filename contains both source and source-version IDs;
- the Markdown target is vault-relative; and
- a PDF citation appends `#page=1`.

Obsidian 1.12.7 is installed on the verification machine. An interactive
click-through was attempted with a temporary test vault, but the Obsidian vault
launcher did not accept automated accessibility actions, so opening the PDF at
the requested page was **not confirmed in the UI**. No existing user vault was
modified during that attempt.

The remaining manual acceptance step is therefore explicit: open the generated
vault in Obsidian, click one source footnote, verify that the copied PDF opens,
and record whether Obsidian honors the `#page=N` fragment on the installed
version. If it opens the PDF but ignores the fragment, the immutable source
link is still valid, but page navigation needs an Obsidian-version-specific
fallback before claiming full manual acceptance.
