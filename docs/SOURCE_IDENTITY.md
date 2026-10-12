# Logical source identity and revisions

Task 2 in `finish.md`, implemented October 12, 2026. These rules apply to
Markdown, TXT, and PDF inputs. Identity belongs to a portable `source_id`,
independent of the original absolute path, basename, or content hash.

## Import rules

| User intent | Command | Result |
| --- | --- | --- |
| Register a separate document | `kc import FILE` | Always creates a new source and initial version. |
| Register several separate documents | `kc import FILE...` | Creates one source per successful input, including duplicate arguments. |
| Update a known document | `kc import --source SOURCE_ID FILE` | Appends a revision if its bytes differ from the current version. |
| Repeat the current revision | Same explicit revision command, unchanged bytes | Returns the current source/version IDs without adding rows. |
| Restore historical bytes, A→B→A | Explicit revision command with A's bytes | Creates a new latest revision; reuses A's retained file. Repeating this command is then idempotent. |
| Rename/move an input | Explicit revision command with the new path | Keeps source identity and display name. Changed bytes record the new basename in the new version. Unchanged bytes return the existing version and its original filename. |
| Import an identical or renamed input as a separate document | Normal import, omitting `--source` | Creates distinct source/version IDs and shares the retained bytes. |

For example, `semester-a/Notes.md` and `semester-b/Notes.md` remain independent
sources. Their current pages can both be selected automatically or individually
with `kc compile --concept TITLE --source SOURCE_ID`.

```bash
kc import semester-a/Notes.md semester-b/Notes.md
# Keep the source IDs returned by this command.
kc import --source src_01J... semester-a/Notes.md
kc extract src_01J...
kc review extraction src_01J...
```

Normal imports never guess update intent. Repeating a normal import creates
another logical source; use `--source` for retry/idempotent revision behavior.
Explicit revisions require exactly one input, an existing active source, and
the same source kind. `.md` and `.markdown` are the same kind. Unknown/archived
sources and kind changes are rejected before modifying logical state. No
filename alias is added automatically: display names remain stable, and the
result reports the version's `original_filename`. Creating a separate renamed
source is an explicit normal import.

## Byte deduplication and historical evidence

Global SHA-256 lookup reuses one read-only retained file for identical bytes.
It never returns a different document's source/version IDs. Each new source or
revision has its own version row, filename, media type, import time, and later
its own extracted page IDs. Shared `stored_path` values may point into an
earlier source/version's directory. Callers use the recorded path rather than
deriving one from IDs. The importer verifies reused bytes against their digest;
it restores a missing retained copy from the staged matching input and rejects
a corrupt copy.

Revisions are ordered by `source_versions.rowid`, under the import write
transaction, so same-second imports do not depend on timestamp ties. Extraction,
extraction review, and new compiler evidence use the latest revision. A newly
created revision needs extraction before its pages can be selected. Old
versions, page IDs, extracted text, proposal citations, article citations, and
vault source copies remain intact. Returning to historical bytes creates new
page IDs on extraction, preserving old reviewed evidence rather than mutating
or silently reactivating it.

Archiving a source cannot remove shared retained files. A future deletion or
cleanup operation must account for every version referencing those bytes.

## Results and partial batches

Successful entries expose these fields in addition to source/version metadata:

| Field | Meaning |
| --- | --- |
| `outcome` | `new_source`, `new_revision`, or `unchanged`. |
| `source_created` | A new logical identity was registered. |
| `version_created` | A new immutable revision was registered. |
| `deduplicated` | Retained bytes were reused; may be true with either creation outcome. |
| `restored` | A new revision restores this source's historical bytes. |

`import_files` continues after per-file failures and returns one ordered
`ImportFileResult` per argument. Each entry contains its input path and exactly
one success or classified failure. Each successful file commits independently.

`kc import good.md missing.md other.md --json` returns one JSON document:

```json
{
  "ok": false,
  "command": "import",
  "error": {
    "code": "partial_import",
    "message": "One or more files failed; see result.files. Successful imports remain committed."
  },
  "result": {
    "partial_success": true,
    "sources": ["<complete good.md metadata>", "<complete other.md metadata>"],
    "files": [
      {"input_path": "good.md", "ok": true, "result": "<complete metadata>"},
      {"input_path": "missing.md", "ok": false,
       "error": {"code": "source_unavailable", "message": "<file-specific diagnostic>"}},
      {"input_path": "other.md", "ok": true, "result": "<complete metadata>"}
    ]
  }
}
```

The metadata placeholders abbreviate actual JSON objects. `result.sources`
contains only successes; `result.files` preserves every input in order.
All-file failure returns `import_failed` with `partial_success: false` and an
empty success list. The exit code is the highest failure code: 2 for invalid
input/configuration, 3 for unavailable input, 6 for state/internal failure.
Human output prints successful IDs to stdout and individual failures to stderr.
Retry only failed inputs to avoid creating duplicate identities for successes.
For intentional revision retries, always supply the original source ID.

## Existing projects

Run `kc init` with this executable to apply migration
`003_source_identity.sql` before importing into an existing project. The
configuration's `schema_version` stays 1; SQL migration history advances to 3.
The migration rebuilds `source_versions` without its global hash uniqueness
constraint and adds hash/source indexes. It preserves IDs, rowids, paths, bytes,
timestamps, and all references. The migration runner temporarily disables
foreign keys for reconstruction, checks every reference before commit, and
restores enforcement on success and rollback.

Previously merged sources remain merged: the old state did not retain enough
input identity to infer which versions were separate documents. Import each
original document normally to register independent identities; retain the old
source/history for existing citations. The migration makes no destructive
guess about old ownership.

## Verification

The regression suite covers explicit revision idempotence, renames, shared
bytes, equal basenames, A→B→A restoration, absent/archived/incompatible revision
targets, partial batches (including corrupt retained bytes), and migration
rollback/reference preservation. A deterministic service journey imports three
equal-basename sources (two with identical bytes), extracts and selects each
independently, compiles a cited proposal, revises A to B's existing bytes and
back to A, then reviews, approves, and applies the historical proposal. It
checks latest extraction/review selection and retained article citations.
Black-box CLI tests verify explicit revision output, restoration/idempotence,
new-source byte reuse, ambiguous revision rejection, and human/JSON partial
success output.

Verified on the macOS arm64 baseline from [Task 1](DEVELOPMENT_BASELINE.md):

| Check | October 12, 2026 result |
| --- | --- |
| `cmake --build --preset dev --parallel 4` | Passed. |
| `ctest --preset dev` | **65/65 passed**. |
| `cmake --build --preset sanitized --parallel 4` | Passed with warnings-as-errors and ASan/UBSan. |
| `ctest --preset sanitized` | **65/65 passed**. |
| `cmake --build --preset release --parallel 4` | Passed. |
| Import CLI regression with release executable | Passed, including equal basenames, byte reuse, revisions/restoration, partial and all-failed batches, and empty revision identity rejection. |
| `git diff --check` | Passed. |

To repeat the release import check:

```bash
cmake \
  -DKC_EXE="$PWD/build/release/apps/kc/kc" \
  -DKC_TEST_ROOT="$PWD/build/release/task-2-import-project" \
  -P tests/cmake/kc_import_test.cmake
```

Local build/test logs are retained in `.cache/task-2/` (ignored by Git). These
are deterministic offline regressions; real OCR/model evaluation remains a
separate finish-plan task.
