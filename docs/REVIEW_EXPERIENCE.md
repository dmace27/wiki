# Exact previews and provenance-preserving review

Task 8 verification: October 13, 2026. Native target: macOS arm64.

## Inspect and approve the actual article

```bash
kc init  # upgrades existing projects through migration 006
kc proposal show PROPOSAL_ID
kc proposal approve PROPOSAL_ID --preview PREVIEW_SHA256
kc apply PROPOSAL_ID
```

`proposal show` includes the destination, exact complete proposed Markdown,
a current-versus-proposed unified diff, validation issues, and every citation's
source evidence. The human output prints a copyable approval command containing
the exact preview token and quoted project path, including when `--project`
selects a project outside the current directory. `proposal preview` provides the
same rendering/diff without the longer proposal/evidence listing. It also
reports malformed stored proposals that cannot be parsed for `proposal show`.

A pure renderer (`libs/vault/src/rendering.cpp`) consumes structured content,
source metadata, the stable article ID, and optional current article bytes. It
has no filesystem or database access. Shared read-only preparation resolves
identity, ownership, current bytes, normalized citations, retained source/image
hashes, and all captured model evidence. Preview and apply call that same
preparation and renderer; apply has no second formatting implementation.

The preview includes actual quoted YAML, user-owned aliases, title, managed
markers, escaped prose, relationships, version-specific source links, PDF page
fragments, and generated footnotes. Existing frontmatter and text outside the
markers appear unchanged in both the rendered article and diff. Create IDs
are deterministic (`art_` plus the proposal's canonical ULID), so inspection
and apply display/write the same metadata without reserving an ID in SQLite.
Previously applied IDs retain their existing identity.

Preview uses a read-only SQLite connection and never creates a database, copies
sources, makes backups, changes review state, or edits an article. It can inspect
an untracked collision without overwriting it. The diff includes additions,
removals, unchanged context, CRLF bytes, and final-newline differences. Its
comparison matrix is capped at four million cells; larger changed middles use
an exact, potentially non-minimal replacement hunk rather than allocating
unbounded quadratic memory.

## Approval binding and changed previews

Approval requires the `--preview` token printed by show/preview. The SHA-256
binds the proposal ID, stable article ID, destination, configuration, exact
before/after bytes, all frozen/cited evidence revisions and digests, and collision
consent. Approval rebuilds and validates the preview in its SQLite transaction;
a missing or changed token fails with exit 5 without approving or writing.
Apply rebuilds and validates it again, then requires the persisted approval
token to match before making any source copy, backup, article, audit, or FTS
change. An old manually marked approval without a token is insufficient.

Changes to source labels/footnote paths, metadata/aliases, configuration,
destination collisions, or article bytes require renewed review. A managed
edit also triggers task 7's stricter approved-base check. Unrelated user additions
remain intact, but **edits after approval require a fresh preview and approval**
of the complete article. This strengthens task 7's managed-base protection;
no failed attempt overwrites those new user bytes.

```bash
kc proposal revoke PROPOSAL_ID
kc proposal show PROPOSAL_ID
kc proposal approve PROPOSAL_ID --preview NEW_PREVIEW_SHA256
kc apply PROPOSAL_ID
```

A token is a content/revision binding, not an authentication secret. It proves
which deterministic rendering was approved; it does not prove semantic
entailment, correctness, or that a human actually read every line. Task 9 now supplies native displaced-byte verification, durable crash recovery,
and reviewed historical restore; see [apply recovery](APPLY_RECOVERY.md).

## Untracked collisions and validation diagnostics

For a deliberate replacement of an untracked user file, inspect the full
removal/replacement diff first:

```bash
kc proposal show PROPOSAL_ID --allow-overwrite-user-file
kc proposal approve PROPOSAL_ID --preview PREVIEW_SHA256 --allow-overwrite-user-file
kc apply PROPOSAL_ID --allow-overwrite-user-file
```

The consent must match at all three stages; an apply-only override cannot
bypass an earlier preview/approval. The displaced bytes are backed up by the
existing writer. The normal collision preview still shows destination,
`before`, `after`, and diff, with `valid: false` and a blocking issue. Empty
untracked files are collisions too.

`proposal show --json` adds `result.preview`. `proposal preview --json` returns
that same object in `result.preview`; invalid previews return exit 5, `ok: false`,
a `validation_failed` error, and the inspectable preview result together.
Missing proposal IDs retain the ordinary resource-not-found error. Existing
article bytes must be UTF-8 without NULs; invalid bytes produce a validation
issue and a digest with `before: null`, rather than a fabricated decoded
preview or a JSON serialization failure. Those original bytes remain untouched.

| Preview field | Meaning |
| --- | --- |
| `proposal_id`, `article_id`, `destination` | Exact proposal/identity and project-relative article path. |
| `valid` | Whether the current rendering can be approved. |
| `untracked_collision` | Whether existing destination bytes belong to an untracked file. |
| `allow_overwrite_user_file` | Consent used to construct this preview. |
| `before`, `after` | Exact UTF-8 article bytes; null denotes absence/unavailable rendering. |
| `before_sha256`, `after_sha256` | Byte digests when content is available, including a digest for invalid existing encoding. |
| `diff` | Complete line-oriented unified diff. |
| `preview_sha256` | Token binding the rendering and inspected inputs. |
| `matches_approval` | True/false if approval exists; null if none or validation failed before comparison. |
| `issues` | Blocking validation paths/messages. |

A rejected/superseded/applied proposal remains available for historical
inspection, but its preview is not approvable. Malformed payloads, unsafe paths,
invalid YAML/markers, changed bases, corrupt/missing sources/images, unreviewed
cited pages, mismatched normalized citations, and stale captured evidence are
reported before approval. The renderer continues the literal Markdown policy
from task 4; model/source text cannot supply its own footnotes or ownership.

## Inspect, open, accept, and correct extraction text

```bash
kc review extraction SOURCE_ID --page 3 --open-image
kc review extraction SOURCE_ID --page 3 --accept
kc review extraction SOURCE_ID --page 3 --text-file corrected-page.txt
```

`--open-image` resolves and verifies the selected image's containment/digest,
opens that exact inspected image in the native default viewer, and prints the
same inspected text/status snapshot in the terminal beside it. Later current
pointer changes cannot pair a different image with that text. It uses a direct argument vector (no shell): `/usr/bin/open`
on macOS, `xdg-open` on Linux, and Explorer on Windows. Native verification is
macOS only; the other platform branches are not advertised as tested targets.
The source/image path is not reconstructed by the user. Human output shows an
absolute image path and a copyable command with the quoted project root.
JSON page records retain `image_path` and add `image_absolute_path` and
`open_image_command`; `result.opened_image` reports the launched image or null.
Pages without images and failed viewer launches return explicit diagnostics.

`--accept` requires `--page` and excludes text replacement options. It records
“reviewed, text unchanged,” preserving every text byte, with a monotonic evidence
revision. Repeated acceptance of already-reviewed unchanged text is idempotent.
It refuses failed/empty extraction text. Acceptance binds the page ID and text
hash inspected by the service so a concurrent correction is not accidentally
accepted. Text replacement still supports visible UTF-8 without NULs.

## Corrections after citation and retained history

Current pages are selected by `source_pages.is_current`. Migration 006 preserves
all existing row IDs, page IDs, text/status/revisions/digests, source versions,
images, proposal evidence, and citation relationships. It replaces the old
unique version/page constraint with a unique index for current rows and adds
`supersedes_page_id` lineage.

An uncited current page may be corrected in place with a new revision/hash.
A page referenced by any proposal citation or captured model context, including
rejected/superseded work, stays immutable: correction creates a new current page
identity linked to its predecessor, sharing the same immutable source version
and verified image. The old row/text/status/hash/offsets remain unchanged.
The result reports `previous_page_id` (null for an in-place correction).

Forced extraction follows the same rule. If current pages are referenced, the
attempt uses new identities and private per-run images, retires the old current
pointers, and publishes new current rows transactionally. Historical images and
citations remain intact, including after a shorter retry. Uncited retries
continue to preserve their stable IDs. Snapshot checks and the SQLite write lock
protect concurrent extraction/correction/generation; an uncited attempt that
becomes cited during its adapters must retry with new identities. New generation
uses only current pages of active/latest source versions. Accumulation can still
retain historical page IDs and exact citations.

For a mistake discovered during review, reject/revoke unused work before
correcting and recompiling it. Existing proposal history remains inspectable;
correction does not silently rewrite an old proposal or applied article. If an
applied claim needs replacement, compile an explicit regeneration and review
its removals, as documented in task 7.

## Review transitions

```text
pending --approve exact preview--> approved --apply--> applied
approved --revoke---------------> pending
pending/approved --reject-------> rejected
pending/approved --new compile--> superseded
```

Revocation clears the captured approval and review timestamp/reason; rejection
records the optional reason and clears any unused approval. Both transitions
run in SQLite transactions and never write the vault. Applied, rejected, and
superseded work cannot be revoked or rejected again. Approval/apply remain
separate actions.

## Verification and reproduction

```bash
cmake --build --preset dev -j 4
ctest --preset dev --output-on-failure -j 4
cmake --build --preset sanitized -j 4
ctest --preset sanitized --output-on-failure -j 4
cmake --build --preset release -j 4
python3 tests/cli/review_experience_smoke.py --kc build/release/apps/kc/kc
python3 tests/cli/accumulated_updates_smoke.py --kc build/release/apps/kc/kc
```

The focused `[review-preview]` C++ regressions cover exact create/update
preview-versus-applied bytes and FTS, no-write preview/approval, deletion diffs,
empty/nonempty collisions and consent, invalid payload/citations/evidence/YAML,
changed user metadata/notes/source labels/configuration, approval revoke/reject,
OCR acceptance/idempotency, verified viewer argv with spaces, historical
correction/new selection after rejection, frozen uncited context, forced
extraction with retained old images, bounded diff behavior, and migration/FK
preservation. Existing two-process extraction/correction/compile tests remain
part of the full suites.

The standalone smoke uses Python standard libraries, disposable notes, a valid
one-pixel PNG, synthetic OCR status, and a loopback-only deterministic model
server. It exercises the actual CLI/curl path for inspect/accept/correct,
rejected citation history, a collision, exact preview approval/application,
regeneration deletion diff, revocation/rejection, stale approval, validation
JSON, and preserved user additions. The native viewer boundary is checked by
an injected C++ runner so tests do not launch the user's GUI applications.
The loopback test requires permission to bind its local socket in a sandbox.

On October 13, 2026, development and ASan/UBSan (warnings-as-errors) suites
each passed **146/146 tests**. Development, sanitized, and release builds passed,
as did both standalone CLI smoke tests against the release executable. Task 8
of `finish.md` records these final results and completed conditions.
These are deterministic correctness tests, not real OCR/model quality or
interactive Obsidian measurements. No other finish task is completed here.
