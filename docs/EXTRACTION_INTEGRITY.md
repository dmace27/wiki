# Extraction integrity and concurrency

Task six verification: October 12, 2026, macOS arm64.

## Path ownership

Every project service validates resolved paths before opening its database.
Initialization performs the same checks before creating configuration, cache
children, or state. Existing path ancestors are resolved with
`weakly_canonical`; containment compares path components, rather than string
prefixes or lexical `..` alone. Dangling symlinks are explicitly rejected:
`weakly_canonical` alone can leave a missing state-file target unresolved,
allowing SQLite to create an external file. Paths containing NULs or invalid
UTF-8 are also rejected before OS calls. A user-selected project root itself may be a
symlink: its canonical destination defines the project boundary.

Sources, vault output, and cache must have separate ownership roots. State may
live directly under cache, as in the default
`.knowledge-compiler/state.sqlite`, but cannot live in sources, the vault,
configuration, or reserved cache children (`pages`, `backups`, `logs`,
`import-staging`, `extraction-staging`). Vault source copies must remain inside
the vault. Overlapping roots, including overlaps introduced by symlinks, are
rejected. Containment is checked again for long-lived service objects and for
specific retained source, staging, rendered-image, and publication paths.

This supports ordinary concurrent compiler processes and detects existing
unsafe layouts. It does not promise protection from an adversarial process
continuously swapping directory entries between filesystem operations. Keep
project directories under the user's control; descriptor-based sandboxing of
hostile writers is outside this task.

## Retained evidence and encoding

Extraction verifies a source against `source_versions.sha256` before invoking
adapters **or returning cached pages**, then verifies it again before publishing
new extraction. Compilation and extraction review also verify retained sources.
Missing, unreadable, escaped, or changed evidence is diagnosed before reuse.
PDF adapters consume a private, verified source snapshot; plain-text extraction
hashes the exact bytes actually read before publication.

Cached page text must match `source_pages.text_sha256`. PDF images must exist
within cache ownership and match `image_sha256`. Compilation checks selected
images before model generation and again during proposal publication; review
checks source, image, and text integrity. Renderer output must stay within its
attempt's staging directory and contain contiguous, nonempty page results.

Markdown/TXT imports, plain-text extraction, native PDF text, OCR text, page
corrections, and typed extracted-page contracts require valid UTF-8 without
embedded NULs. Valid Unicode, line endings, tabs, and UTF-8 BOMs are preserved;
there is no silent transcoding or normalization. Citation offsets remain byte
offsets into the original stored UTF-8 text. SQLite readers use
`sqlite3_column_bytes`, so legacy NULs cannot truncate text before validation,
hashing, or citation checks.

Restore a missing or damaged source from a known-good copy with its recorded
SHA-256. Reimporting identical bytes can repair missing shared source storage;
changed retained bytes are never silently accepted as a revision. Import a new
explicit revision when the original material changes. Repair uncited images
with `kc extract --force SOURCE_ID`; extraction rejects attempts to change
already captured proposal evidence.

## Concurrent extraction and correction

Each extraction attempt renders into a unique staging directory and publishes
images under `cache/pages/VERSION_ID/RUN_ID/`. It never overwrites another
attempt's images. Normal failures remove only that attempt's output and
staging. Previous published images remain intact, even after an uncited forced
replacement. Garbage collection and crash cleanup remain task nine's work.

Extraction captures all existing page state in one SQLite read transaction,
including a monotonic `evidence_revision`. After rendering/OCR, its
`BEGIN IMMEDIATE` publication transaction verifies the original snapshot and
checks both normalized citations and captured proposal context. A competing
extraction, correction, or newly published proposal makes the attempt fail
with a retry diagnostic. No page row or earlier image is replaced. Stable page
IDs survive successful force extraction; rows for removed uncited pages are
deleted when a shorter document is extracted.

Corrections increment `evidence_revision` in the same write transaction as the
text, hash, and reviewed status. Forced extraction also increments revisions.
An A → B → A correction therefore cannot appear unchanged.

## Proposal publication

Evidence selection captures page identity, source version, number, image path,
exact text, status, revision, text/image digests, source identity/name, retained
path, and source digest from the same SQLite query that supplies generation.
After model generation, proposal publication rechecks **every selected page**
under its write transaction. The source must still be active and its selected
version must still be latest. Retained file/image hashes are rechecked there.

A stale generation publishes no proposal, citation, or evidence row and cannot
supersede an existing proposal. Its model-run audit remains a record of the
completed generation; that audit does not imply publication. Review changed
evidence and compile again; no automatic retry hides this boundary.

Successful publication records every supplied page in `proposal_evidence`,
including context the model did not explicitly cite. Both correction and
re-extraction freeze these pages, using the existing immutable-proposal policy
for pending, approved, rejected, superseded, and applied history. Importing a
new source revision remains supported and leaves old evidence intact.

## Upgrade

Run `kc init` on existing projects to apply migration
`004_extraction_integrity.sql`. It preserves existing page IDs, text, citations,
and proposals, initializes page revisions to zero, and adds the internal image
hash and proposal-evidence records. Public JSON schema versions stay at one.
New compilations populate the evidence table; older proposals retain their
existing normalized citation protection.

Legacy image hashes are NULL because migration cannot establish an original
image's integrity from SQL. Such images produce an explicit integrity error,
never a newly invented trusted digest. Force-extract uncited versions to
establish fresh images and hashes. Historical cited versions remain frozen;
preserve their source/text/citation history and use a new explicit source
revision for further compilation. Migration does not delete or bless legacy
images.

## Reproduce verification

```sh
cmake --build --preset dev -j 4
ctest --preset dev --output-on-failure
build/dev/tests/kc_tests '[integrity]'
cmake --build --preset sanitized -j 4
ctest --preset sanitized --output-on-failure
cmake --build --preset release -j 4
```

| Final check | Result |
| --- | --- |
| Development build and CTest | 119/119 passed. |
| ASan/UBSan build and CTest, warnings-as-errors | 119/119 passed. |
| Isolated `[integrity]` suite | 14 test cases, 172 assertions passed. |
| Release build | Passed. |
| Release `kc_extract_test.cmake` | Retained-copy extraction and cache reuse passed. |
| Release CLI smoke | Migration 004/idempotency, Unicode import/extraction/cache reuse, tampered-source rejection, and NUL-input rejection passed. |

Verification used the established local dependency installation. CMake was
configured with `VCPKG_MANIFEST_INSTALL=OFF` to avoid an unnecessary write to
the dependency installation's lock outside the workspace sandbox. No dependency
or toolchain changes were made for this task.

The release retained-copy regression is reproducible with:

```sh
cmake -DKC_EXE="$PWD/build/release/apps/kc/kc" \
  -DKC_TEST_ROOT=/tmp/kc-task6-release-extraction \
  -P tests/cmake/kc_extract_test.cmake
```

The integrity suite uses isolated temporary projects with real SHA-256 records.
It covers initialization/storage/cache/staging/state symlink escapes,
overlapping ownership, missing/tampered source bytes, missing/tampered/legacy
images, invalid UTF-8 and embedded NULs, SQLite byte-length handling, and
uncited-context freezing.

On the advertised native macOS target, pipe barriers coordinate actual forked
processes without timing-dependent sleeps. Tests pause generation or rendering,
commit a competing process's correction, extraction, import revision, or
compilation, then resume publication. They verify rejected stale work, unchanged
published images, active image hashes, failed-attempt audit state, empty staging,
and no stale proposal/citation/evidence rows. Adapter/model doubles isolate the
race boundary; these are not real OCR or model-quality measurements.
