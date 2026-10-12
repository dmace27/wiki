# Knowledge Compiler

Knowledge Compiler turns PDFs, scans, and Markdown learning notes from any
subject into a personal, source-grounded Obsidian knowledge base. The MVP uses
one trustworthy, repeatable loop to compile any requested concept into a cited,
reviewable article.

## Foundation build

The project uses C++23, CMake, Ninja, and vcpkg manifest mode. The supported
development target for this baseline is **macOS on Apple Silicon** with the
`arm64-osx` triplet. Other platforms still need their own clean verification
before being advertised as supported.

| Tool | Requirement | Verified version |
| --- | --- | --- |
| Compiler / standard library | Apple Clang 21 with matching Xcode Command Line Tools; C++23 | Apple Clang 21.0.0 (`clang-2100.1.1.101`) |
| CMake | 3.25 or newer (preset schema 6 and `--fresh`) | 4.4.0 |
| Ninja | 1.11 or newer | 1.13.2 |
| Git | 2.x; required by vcpkg versioned manifests | See the [baseline verification](docs/DEVELOPMENT_BASELINE.md) |
| vcpkg checkout | Exact commit below, matching `vcpkg.json` | Tool release 2026-04-08 |

Install the Command Line Tools with `xcode-select --install` if needed. Install
CMake, Ninja, and Git, for example with `brew install cmake ninja git`, and
confirm `clang++ --version`, `cmake --version`, `ninja --version`, and
`git --version` before configuring. PDF/OCR programs and Ollama are not needed
to build or run the deterministic tests.

From the repository root, create a dedicated **persistent** vcpkg checkout.
The Git checkout pins both port definitions and bootstrap tool metadata; the
manifest's `builtin-baseline` pins dependency versions. Keep both pins together
when deliberately upgrading dependencies. Use a full clone, not a shallow
clone, so vcpkg can read the versioned port history.

```bash
export VCPKG_ROOT="$HOME/.local/share/knowledge-compiler/vcpkg"
mkdir -p "$(dirname "$VCPKG_ROOT")"
git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
git -C "$VCPKG_ROOT" checkout --detach d015e31e90838a4c9dfa3eed45979bc70d9357fc
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
```

For an existing dedicated checkout, skip `git clone`, fetch its history with
`git -C "$VCPKG_ROOT" fetch origin`, then repeat checkout and bootstrap. Export
`VCPKG_ROOT` in every new shell, or save that export in your shell configuration.
Do not put this checkout under `/tmp` or inside `build/`.

The first configure downloads and compiles the manifest dependencies and needs
internet access. The presets install them into `$VCPKG_ROOT/installed-kc`,
shared across the three configurations and retained when build directories are
recreated. Downloads and dependency build trees also stay under `VCPKG_ROOT`.
No global `vcpkg integrate install`, existing CMake cache, or manually supplied
`CMAKE_PREFIX_PATH` is required. To reproduce the source-only dependency check,
set `export VCPKG_BINARY_SOURCES=clear` before the first configure; normal
development may use vcpkg's default binary cache.

Configure from fresh state, build, and run the development tests:

```bash
cmake --fresh --preset dev
cmake --build --preset dev --parallel 4
ctest --preset dev
```

For warnings-as-errors with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --fresh --preset sanitized
cmake --build --preset sanitized --parallel 4
ctest --preset sanitized
```

Build the optimized executable separately:

```bash
cmake --fresh --preset release
cmake --build --preset release --parallel 4
```

`release` intentionally disables test targets; test results come from `dev` and
`sanitized`. All presets select the native triplet (`arm64-osx` on the supported
host); an explicit triplet override requires a fresh configuration and matching
compiler architecture. `--fresh` discards old CMake cache/toolchain paths while
retaining installed dependencies. For a completely empty application build
tree, rename the corresponding `build/dev`, `build/sanitized`, or
`build/release` directory first. Never copy another developer's build cache.

If configuration refers to `/scripts/buildsystems/vcpkg.cmake`, `VCPKG_ROOT`
was not exported. If it refers to an old `/tmp` path, export the persistent root
and run the `--fresh` configure command again. Use the same compiler and SDK for
all three presets; after changing them, reconfigure each preset so vcpkg can
rebuild dependencies for the new compiler ABI.

ICU is **not** part of this baseline: SQLite enables FTS5 and default JSON1,
without its optional `unicode` feature. The pinned SQLite port otherwise makes
an unnecessary optional ICU lookup for static libraries; the project disables
that lookup. Concept identity uses the manifest's utf8proc dependency for
NFC normalization and full case folding, independently of SQLite's `NOCASE`.
Curl must come from the manifest's CMake config package, rather than the macOS
system SDK. CMake 3.29+ uses linker-aware archive deduplication (CMP0156), with
first-occurrence ordering on 3.31+ (CMP0179). CMake 3.25–3.28 can still emit
Apple linker warnings about repeated static libraries; those repetitions are
dependency ordering, not duplicate object definitions. Prefer the verified
CMake version for a warning-free build.

See [the dated clean-build evidence](docs/DEVELOPMENT_BASELINE.md) for dependency
versions, test results, and the release smoke check.

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

Each normal import creates a **new logical source**, even for the same filename
or identical bytes. SHA-256 deduplication reuses immutable project-local bytes
without merging source identities. Keep the returned source ID to explicitly
revise that document:

```bash
./build/dev/apps/kc/kc import --source src_01J... "./Updated Notes.pdf"
```

Explicit revisions preserve the source's display name and kind. Repeating its
current bytes is idempotent; changed bytes create a new latest version. Returning
to historical bytes also creates a new latest version and preserves all old
citations. Multi-file imports continue after individual failures and report
all successes and errors, returning a nonzero exit code if any file fails.
For existing projects, run `kc init` once to apply migration 003 before importing
with this version. See [source identity rules](docs/SOURCE_IDENTITY.md) for
renames, shared storage, batch JSON, and migration details.

Extract the latest immutable version using the source ID returned by import:

```bash
./build/dev/apps/kc/kc extract src_01J...
```

Markdown and TXT sources produce one native logical page. PDF extraction uses
the local `pdftoppm` and `pdftotext` programs, retains a PNG for every page, and
falls back to local Tesseract OCR when native text is unusable. Install those
programs on `PATH` to process PDFs; project initialization does not require
them. Use `--force` to retry an existing extraction.

Review extraction evidence before compilation. The terminal surface keeps the
rendered image path, extracted text, and extraction status together. Correct a
page inline or from a UTF-8 file; either form records a new text hash and marks
the page `reviewed`:

```bash
./build/dev/apps/kc/kc review extraction src_01J...
./build/dev/apps/kc/kc review extraction src_01J... --page 3 \
  --text-file corrected-page.txt
```

Corrections are intentionally refused after a proposal cites the page, so
proposal citation offsets and quotes remain immutable.

The local model layer uses the Ollama settings in `kc.json` to request a
schema-constrained proposal for the selected concept. It validates the exact
resolved canonical display title, its new or stored slug, and all returned citations
against supplied page text and records a redacted `model_runs` audit entry.
Compile relevant extracted pages into a pending proposal with:

```bash
./build/dev/apps/kc/kc compile --concept "Markov Chains"
```

Add one or more `--source src_01J...` options to restrict the evidence set.
Compilation deterministically matches the requested title and its meaningful
terms across source names and page text. If a concept is only implicit, passing
one or more `--source src_01J...` options treats those sources as curated
evidence when no lexical match exists. The compiler stores normalized citations
transactionally and never writes vault files. Invalid model output remains only
a redacted failed model run and cannot become a pending proposal.

Configuration, extracted pages, citations, and proposals share
[strict JSON contracts](docs/JSON_CONTRACTS.md). Integer fields reject decimal
and exponent notation, booleans, strings, negatives, and out-of-range values
before conversion. Every proposed section must contain meaningful cited prose;
repeated section keys remain supported for multiple examples.

Case/Unicode/whitespace variants update the same concept while preserving its
display title, slug, and filename. Slug collisions and nonportable titles fail
before model generation. See [concept identity rules](docs/CONCEPT_IDENTITY.md)
for Windows device names, Unicode behavior, legacy projects, and language limits.

Approve a reviewed proposal, then apply it as a separate operation:

```bash
./build/dev/apps/kc/kc proposal list --status pending
./build/dev/apps/kc/kc proposal show prp_01J...
./build/dev/apps/kc/kc proposal approve prp_01J...
./build/dev/apps/kc/kc apply prp_01J...
```

`proposal show` displays the proposed sections followed by each citation's
source page evidence. A pending proposal can instead be rejected with
`proposal reject`; list, show, approve, and reject never edit an article.

Approval never changes the vault. Application verifies and copies every cited
immutable source, renders page-linked footnotes, writes the article through an
atomic temporary-file replacement, and updates audit/citation/search state only
after that replacement succeeds. Existing generated articles retain every byte
outside their `kc:managed` markers, and the complete previous file is backed up
under `.knowledge-compiler/backups/`.

New articles use quoted YAML strings, an actual empty alias list when needed,
and no inferred tags. Aliases become user-owned after creation; updates preserve
frontmatter and index the aliases actually stored there. Generated text renders
literally, with the writer supplying related links and source footnotes. See
[Markdown metadata and rendering](docs/MARKDOWN_METADATA.md) for supported text,
legacy metadata, and validation rules.

If a create proposal collides with an untracked `<Concept title>.md`, `kc apply`
refuses to overwrite it. After reviewing that exact collision, the explicit
`--allow-overwrite-user-file` flag permits replacement and still creates a
recovery backup.

Search applied articles locally by title, alias, and generated Markdown body:

```bash
./build/dev/apps/kc/kc search "markov chains"
./build/dev/apps/kc/kc --json search "transition probability" --limit 5
```

Results include the article title and project-relative vault path. Title and
alias matches rank ahead of body-only matches, and the query is treated as
literal keywords rather than raw SQLite FTS syntax.
