# Concept identity and portable titles

Implemented October 12, 2026 for task 3 in `finish.md`.

## Identity and display

A concept's identity key is its title with horizontal whitespace normalized,
Unicode NFC normalization, and full locale-independent Unicode case folding.
The pinned manifest dependency **utf8proc 2.11.3** supplies the Unicode rules;
SQLite `NOCASE` is not used to resolve concepts. Accents and punctuation remain
significant. For example:

| Requests | Identity |
| --- | --- |
| `Photosynthesis`, `photosynthesis`, ` PHOTOSYNTHESIS ` | One concept. |
| `Cellular  Respiration`, `Cellular<TAB>Respiration` | One concept. |
| `Café`, `CAFÉ`, `Cafe` plus U+0301 | One concept. |
| `Straße`, `STRASSE` | One concept (full case folding). |
| `Биология`, `БИОЛОГИЯ` | One concept. |
| `Résumé`, `Resume` | Distinct concepts. |
| `A-B`, `A B` | Distinct identities, but a slug collision (see below). |

New display titles use NFC and single ASCII spaces: leading/trailing Unicode
space separators are trimmed; internal runs of those separators and tabs are
collapsed. Line breaks, Unicode control/format characters, embedded NULs, and
malformed UTF-8 are rejected. Normalized titles must occupy 1–200 UTF-8 bytes.
Compatibility normalization, transliteration, locale-specific casing, aliases,
and semantic equivalence do not establish identity. In particular, full case
folding does not impose Turkish locale-specific rules.

An applied article retains its **article ID, exact stored display title, slug,
and recorded filename/path**. Compile resolves this identity before selecting
evidence or calling the model, supplies its exact title and slug in the prompt,
and requires the response to retain them. A historical Unicode hash slug is
not regenerated during an update. Pending create proposals reuse the first
pending display spelling; a newer proposal supersedes only the same identity,
including approved-but-unused work. Other concepts remain pending.

Approval and apply check the same current identity. Case-only legacy update
payloads are read/rendered with the stored display title; their immutable JSON
history is preserved. Search returns that canonical title and recorded path.
Identity claims and destination ownership are checked again under the proposal
publication/apply write transaction, including changes made during generation.

## Slug and destination collisions

Slugs remain ASCII kebab-case. ASCII letters/numbers are retained, apostrophes
are omitted, other punctuation separates words, and `+`, `#`, `&`, `@` become
`plus`, `sharp`, `and`, `at`. Titles without ASCII letters/numbers after case
folding receive a stable FNV-1a `concept-<16 hex digits>` slug over the identity
key. A hash is a naming fallback, not a uniqueness guarantee.

**Collision policy: reject and ask for a more specific title.** There is no
implicit merge or automatic suffix. `A-B` and `A B` both produce `a-b`; once one
is applied or pending/approved, compiling the other fails with `invalid_concept`
and a diagnostic naming both concepts **before any model call or audit run**.
Choose a title such as `A B Theory`, or reject conflicting unused pending work
before compiling again. Rejected/superseded proposals do not reserve slugs.
The same checks guard apply if conflicting state appears after approval.

A destination already recorded for another article is also rejected before
compilation and at apply, even with `--allow-overwrite-user-file`. On
case-sensitive filesystems apply refuses a sibling filename that would alias
under Unicode/case rules on another platform. The overwrite flag applies only
to an untracked user file at the permitted destination.

## Portable filenames

New articles use `<normalized display title>.md`. The title validation runs at
compile time, in model/domain proposal validation, and before approval/apply.
These component rules are independent of the host operating system:

- Reject `/`, `\`, `<`, `>`, `:`, `"`, `|`, `?`, `*` and control/format characters.
- Reject titles ending in a dot after whitespace normalization, including `.`
  and `..`; trailing horizontal whitespace is normalized away for new titles.
- Reject Windows device names case-insensitively, even before an extension:
  `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, `CONIN$`, `CONOUT$`.
  Superscript digits ¹/²/³ are also reserved for COM/LPT; spaces before a device
  name's extension are conservatively rejected.
- Bound the UTF-8 title to 200 bytes, leaving room for `.md` and temporary-file
  suffixes within common 255-byte/UTF-16 component limits.

`C++`, `C#`, apostrophes, and non-Latin titles are supported. Article database
paths and CLI output use explicit UTF-8 conversions; native filesystem paths
use the platform encoding (UTF-16 on Windows). Temporary article paths preserve
the native filename rather than round-tripping through a local code page.
These are portable component rules; project-root length, configured directory
names, filesystem permissions, and OS support remain separate constraints.
The advertised/verified target remains macOS arm64. This task does not advertise
unverified Windows/Linux binaries or claim native testing on those systems.

## Retrieval language support

Automatic evidence selection NFC-normalizes and case-folds Unicode text,
retains letters/numbers/combining marks from all scripts, and compares title
phrases or all meaningful title terms at whitespace/punctuation boundaries.
Programming symbols use the same named words as before. The small ASCII `s`
heuristic and English function-word list are retained; there is no general
stemming, translation, dictionary segmentation, or semantic search.

French accents, Cyrillic capitalization, and standalone non-Latin title phrases
match. A title embedded in an unbroken Chinese/Japanese/Thai sentence may need
curation, as may inflected or implicitly described concepts. Use:

```bash
kc compile --concept "量子力学" --source SOURCE_ID
```

The existing `no_evidence` diagnostic explicitly suggests `--source`. Empty
normalized phrases never match empty/punctuation-only pages. Punctuation-only
or symbol-only titles can still be compiled from curated sources. Normalized
copies are used only for ranking; citation offsets retain the original UTF-8
page bytes.

Search checks the **exact identity key first in result ordering**, so case and
NFC variants (including `Straße`/`STRASSE`) find the canonical page even when
FTS5's token folding differs. This also supports exact punctuation-only concept
names. Free-text title/alias/body retrieval still uses literal AND terms and
SQLite `unicode61 remove_diacritics 2`; it may return accent-insensitive or
punctuation-separated matches for distinct concepts. Such retrieval matches
never merge identities or redirect updates. Invalid UTF-8 queries are rejected.

## Existing projects

No schema migration is needed for the rejection policy. Existing schema-3
article/proposal rows are compared in C++ using the shared rules. The previous
SQLite constraints remain secondary safeguards; no IDs, paths, citations, or
approved article bytes are rewritten merely by opening a project.

If legacy data contains multiple articles equivalent under Unicode folding or
NFC, compile and exact search report an ambiguous identity and preserve both.
There is no automatic merge/deletion. Back up the project and curate those
records/pages before updating that concept; a general rename/merge repair CLI
is outside this task. Legacy unsafe filenames produce validation diagnostics
rather than creating another platform-dependent page. Unapplied legacy slugs
that differ from the new Unicode rules require recompilation; their old
proposal history remains available and the replacement uses the canonical slug.

## Verification

The deterministic regression suite covers create → review → approve → apply →
case/whitespace/Unicode update → search for biology, accented Latin, Cyrillic,
Chinese, and punctuation-only concepts; pending Unicode supersession;
applied/pending slug collisions without model calls; collisions introduced
during generation or after approval; tracked destination protection; canonical
rendering of legacy case-only updates; historical Unicode slugs/custom paths;
ambiguous old Unicode rows; portable filename rejection; strict invalid UTF-8
and NUL title handling; non-English lexical matches; and explicit CJK curation.

Tests reside in `tests/unit/concept_identity_tests.cpp`,
`tests/integration/compiler_tests.cpp`, and
`tests/integration/vault_writer_tests.cpp`. The CLI compile regression also
checks portable-title errors. Run:

```bash
cmake --build --preset dev --parallel 4
ctest --preset dev
cmake --build --preset sanitized --parallel 4
ctest --preset sanitized
cmake --build --preset release --parallel 4
```

Final measured results are recorded in task 3's completion entry in `finish.md`.
Local configure/build/test logs are under ignored `.cache/task-3/`. All model
journeys use deterministic injected responses; no real-model quality or other
finish-plan task is implied by these identity tests.

The normalization APIs follow the
[utf8proc API reference](https://juliastrings.github.io/utf8proc/doc/utf8proc_8h.html).
The portable device-name/component rules follow
[Microsoft's filename rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
