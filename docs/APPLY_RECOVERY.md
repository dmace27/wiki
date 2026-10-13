# Durable apply, recovery, and reviewed restoration

Task 9 implementation and native verification: October 13, 2026, macOS arm64.
Stop older compiler processes, then upgrade existing projects with `kc init`
(migration 007). Pre-upgrade approvals require a fresh preview under the new
publication-event binding. This task covers file /
SQLite crash consistency; it does not complete installation, workload limits,
search rebuilding, or other finish tasks.

## Supported workflow

```bash
kc history                     # all applied revisions and restore audit records
kc history ARTICLE_ID          # restrict to a concept
kc restore APPLY_RUN_ID        # inspect exact historical bytes and removal diff
kc restore APPLY_RUN_ID --preview SHA256_FROM_INSPECTION
kc recover                     # automatic recovery plus retained-artifact inventory
```

Restore inspection is read-only after any necessary project-open recovery.
Executing restore requires its exact current preview token. The token binds the
target snapshot, active revision, last committed apply/restore event, configuration, immutable evidence, and every
current article byte (including user metadata/notes). An edit after inspection
invalidates the token. The diff explicitly shows deletion of current user edits;
there is no unreviewed force flag. Even an A→B→restore-A cycle cannot resurrect
an old approval/restore token merely because the bytes match again. The entire current article is backed up before
restoration. A missing destination can be restored, but a concurrently created
file cannot be clobbered in the final publication gap.

A restore selects an exact **post-apply** snapshot. It reinstates the full
Markdown, normalized citations from that applied proposal, historical aliases
and FTS body, content digest, and active structured apply revision together.
It appends a `restore_runs` audit record; original applies/proposals/citations
remain immutable history. Later compilation accumulates from the restored
revision, and proposals based on a different revision fail their base checks.
Restore uses the same journal/backup/exchange/commit protocol as apply.

New applies record exact full snapshots in `article_snapshots`. Migration 007
sets the active pointer for legacy applied articles but does not invent their
historical bytes. Such history has `restorable: false`; old backups are retained
for manual inspection, and subsequent applies create restorable snapshots.
Historical source paths and ownership markers require the original vault
rendering configuration; restore rejects a changed source directory/marker
configuration instead of writing old footnotes pointing to new paths. Retained
sources/images and citation offsets are verified again before restore.

JSON `history` exposes `revisions` (run/proposal/article IDs, timestamp, digest,
backup, `current`, `restorable`) and `restores` (restore ID, from/to apply IDs,
timestamp, pre-restore backup). JSON restore inspection exposes complete
`before`, `after`, `diff`, destination, current/target IDs and `preview_sha256`.
Executing restore returns its audit run ID. `recover` exposes retained artifacts
and conflict archive paths. Commands use the usual JSON envelope and error exits.

## Commit protocol

The writer holds `BEGIN IMMEDIATE` from approval validation through publication.
The filesystem journal lives at `cache/transactions/RUN_ID.json`; it is synced
before changing backups, source copies, or articles. It records exact before/
after bytes and SHA-256 digests, the destination and temporary sibling, backup,
and only those immutable source copies that did not already exist.

| Durable step | Recovery after process death |
| --- | --- |
| Journal staged, before publication | Remove recognized `.json.pending`; no vault mutation can have happened. |
| Published journal, backup, source staging/copies, article staging | SQLite has no commit marker: remove owned staging/new copies/backup, preserve the prior article. |
| Journal `replacing`, before/after native exchange | SQLite has no commit marker: undo replacement using displaced bytes or journal before bytes. An absent pre-apply file returns to absence. |
| Article replaced, SQL prepared, before COMMIT | Roll back the same way; article/citations/proposal/audit/FTS SQL remains unpublished. |
| SQLite COMMIT completed | `file_operations` exists: retain article/source copies/backup/history and finish journal/temp cleanup. |
| Recovery itself interrupted | Repeat these checks idempotently; restored bytes or absence are recognized. |

The `file_operations` commit marker is inserted in **the same SQLite transaction**
as article identity/digests, proposal application state, citations, audit,
snapshot/active pointer, and FTS. It is the authority for commit versus rollback;
a mutable journal phase alone never claims SQL committed. Caught errors use the
same restart protocol after SQLite unwinds. Cleanup errors after commit may
return failure, but restart/history reports the actual committed operation;
there is no second application or rollback of committed history.

Recovery runs when opening a project's state connection, including before a
read-only inspection sees interrupted work. Normal read-only previews do not
open a recovery writer when no interrupted work exists. Recovery serializes
with live writers under SQLite's write lock. It validates containment and
operation-owned paths and rejects symlinks/escaping or corrupt journals before
following their recorded destinations. Journals contain private article text;
retain them inside the private project cache.

## External editors and conflicts

Before replacement, the writer rechecks exact current bytes after all potentially
slow backup/source staging. Existing articles use native atomic **exchange**:
macOS `renameatx_np(RENAME_SWAP)` (Linux implementation: `renameat2(RENAME_EXCHANGE)`).
The staged name then contains the exact displaced article. Comparing those bytes
with the approved before image detects an editor's atomic save in the final
read/rename gap. A new destination uses exclusive rename, so an editor's new
file wins rather than being overwritten. The writer also checks article bytes
before SQL COMMIT. SQLite locks do not imply locks on an external editor.

If an editor saved in that gap, rollback restores the actual displaced user
bytes, not an obsolete preview. If the destination changed again after the
exchange, recovery leaves it untouched and retains the journal/staged/before/
after bytes. Open/search operations report an actionable recovery conflict
instead of presenting an interrupted apply as successful. Repair permissions/
locks or restore the expected bytes and retry `kc recover`.

To explicitly keep a conflicting current user file:

```bash
kc recover --keep-user-file
kc history ARTICLE_ID
kc restore APPLY_RUN_ID        # review any subsequent deliberate restoration
```

The keep action archives the journal plus exact kept/displaced binary bytes at
`cache/recovery-conflicts/`, then removes its active journal/temp artifacts. It
preserves the current article and retained sources/backups; SQLite remains at
the last committed state. This deliberately acknowledges an external edit, not
an apply: it cannot silently publish model content/citations or claim the kept
file is an approved snapshot. Review a restore or manually reconcile managed
content before compiling. Invalid UTF-8 user bytes can be kept without decoding.
Unknown legacy temporaries/unreferenced backups are reported and preserved,
rather than removed by a filename heuristic. Historical/unowned source copies
are retained for citation and future restore safety. Newly journal-owned copies
and backups from a rolled-back apply are removed automatically.

An editor holding a displaced inode can write after exchange. Unexpected bytes
there are retained in the conflict archive at committed cleanup, instead of
being discarded. No protocol can prevent arbitrary noncooperating edits after
the final verification, or make SQLite and filesystem reads appear atomic to
an editor. Writes after the publication boundary are external edits of a
committed article; managed-base and exact-preview checks guard the next update.
The protocol preserves observed saves and fails closed on ambiguous recovery.

## Durability guarantees and system boundaries

On the supported macOS arm64 target, complete journal, backup, article staging,
and source-copy data is synchronized with `fsync` and `F_FULLFSYNC` before
publication. Files are exclusively created with no-follow semantics; short
writes, ENOSPC, sync failure and permission failures propagate. Newly created
directories and parent directory entries are synced after creation, rename,
exchange and removal. Filesystem changes precede SQLite COMMIT. SQLite
`synchronous=FULL`, `fullfsync=ON`, and `checkpoint_fullfsync=ON` protect the
commit marker/WAL publication. Backups and snapshots retain the exact original
bytes; partial backups matching a prefix can be removed on rollback because
the complete durable journal already contains those bytes.

These are local-filesystem/device flush guarantees subject to the filesystem
and hardware honoring sync. The tests terminate processes at exact boundaries;
they do not physically cut power or establish guarantees for network drives,
failing hardware, cloud sync, or unsupported filesystems. Failure to perform a
required exchange/sync is an error, never a fallback to unsafe rename. Linux
has implementation branches but no native verification here. Windows existing-
article exchange is intentionally unsupported until equivalent native semantics
are tested; Windows is not an advertised release target.

## Interrupted extraction and reconciliation

Every extraction attempt holds an OS lease in `cache/extraction-leases/RUN.lock`
from before its `running` row until its adapters/publication finish. Locks vanish
on process death. Another opener tries a nonblocking lease: live adapters stay
running with their files intact, without relying on age or PID-reuse guesses.
An abandoned run is marked failed with a completion timestamp and retry message.
Its private adapter staging is removed; per-run images are removed only if no
current **or historical** page references them. Previously cited images survive.
Empty lock files remain as ownership records (unlinking a locked inode could
create a second independent lock). Retry proceeds through normal extraction.

## Verification

```bash
cmake --build --preset dev -j 4
ctest --preset dev --output-on-failure -j 4
cmake --build --preset sanitized -j 4
ctest --preset sanitized --output-on-failure -j 4
cmake --build --preset release -j 4
python3 tests/cli/apply_recovery_smoke.py --kc build/release/apps/kc/kc
python3 tests/cli/review_experience_smoke.py --kc build/release/apps/kc/kc
python3 tests/cli/accumulated_updates_smoke.py --kc build/release/apps/kc/kc
```

The `[apply-recovery]` tests `_exit` children before/after each durable operation:
19 create/collision boundaries, 17 update and restore boundaries, and seven
recovery boundaries. Restart checks full Markdown, approval/application status,
active audit revision, exact citations, snapshots, FTS, source-copy retention,
and removal of owned temporary artifacts. Other cases cover external atomic
saves before/during exchange and before COMMIT, exclusive-create races,
displaced inode writes, binary conflict preservation, malformed/escaping journal
paths, stale restore tokens, changed historical evidence/configuration,
legacy/unowned artifact inventories, A→B→restore-A approval protection, and
live versus killed extraction adapters.

Failure cases include injected ENOSPC/EACCES/EBUSY and partial backup/staging
writes, native directory permission denial (including failed restoration), a
native macOS immutable file lock, aborted SQL publication, and SQLite's actual
commit-hook rejection after all SQL/file work. Fault injection uses explicit
C++ observers; production has no environment-enabled crash backdoor. Disk-full
cases are deterministic injected errors/short files, not filling the user's
disk. CLI smoke uses a loopback deterministic model fixture and disposable
sources; it measures CLI/transaction correctness, not OCR/model usefulness.

Final verification on October 13, 2026: development and ASan/UBSan with warnings
as errors each passed **164/164 tests** against the final unchanged binaries.
The release build and all three standalone CLI smokes passed. The additional
concurrent opener test checks committed journal cleanup under writer
serialization. Task 9's checklist and finish criteria are complete; no other
finish task was completed.
