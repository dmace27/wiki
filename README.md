# Knowledge Compiler

Knowledge Compiler turns PDFs, scans, and Markdown learning notes into a personal,
source-grounded Obsidian knowledge base. The MVP targets one trustworthy loop:
compile probability notes into a cited, reviewable `Markov Chains.md` article.

## Foundation build

The project uses C++23, CMake, and vcpkg manifest mode. Install CMake, Ninja, and
vcpkg, set `VCPKG_ROOT`, then run:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For warnings-as-errors with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized
```

Initialize a project without starting Ollama or Tesseract:

```bash
./build/dev/apps/kc/kc --project /path/to/project init --vault vault
```

The command creates `kc.json`, project-local storage directories, and applies
all pending SQLite migrations transactionally.

Import Markdown, text, or PDF sources from the project directory (or pass
`--project` explicitly):

```bash
./build/dev/apps/kc/kc import "./Probability Notes Week 8.pdf"
```

Each import is SHA-256 hashed and copied into immutable project-local storage.
Re-importing unchanged bytes is idempotent; changed content creates a new source
version without overwriting the earlier retained file.

Extract the latest immutable version using the source ID returned by import:

```bash
./build/dev/apps/kc/kc extract src_01J...
```

Markdown and TXT sources produce one native logical page. PDF extraction uses
the local `pdftoppm` and `pdftotext` programs, retains a PNG for every page, and
falls back to local Tesseract OCR when native text is unusable. Install those
programs on `PATH` to process PDFs; project initialization does not require
them. Use `--force` to retry an existing extraction.

The local model layer uses the Ollama settings in `kc.json` to request a
schema-constrained Markov Chains proposal. It validates all returned citations
against supplied page text and records a redacted `model_runs` audit entry.
Compile relevant extracted pages into a pending proposal with:

```bash
./build/dev/apps/kc/kc compile --concept "Markov Chains"
```

Add one or more `--source src_01J...` options to restrict the evidence set.
Compilation deterministically matches title, alias, and topic keywords, stores
normalized citations transactionally, and never writes vault files. Invalid
model output remains only a redacted failed model run and cannot become a
pending proposal.

Approve a reviewed proposal, then apply it as a separate operation:

```bash
./build/dev/apps/kc/kc proposal approve prp_01J...
./build/dev/apps/kc/kc apply prp_01J...
```

Approval never changes the vault. Application verifies and copies every cited
immutable source, renders page-linked footnotes, writes the article through an
atomic temporary-file replacement, and updates audit/citation/search state only
after that replacement succeeds. Existing generated articles retain every byte
outside their `kc:managed` markers, and the complete previous file is backed up
under `.knowledge-compiler/backups/`.

If a create proposal collides with an untracked `Markov Chains.md`, `kc apply`
refuses to overwrite it. After reviewing that exact collision, the explicit
`--allow-overwrite-user-file` flag permits replacement and still creates a
recovery backup.
