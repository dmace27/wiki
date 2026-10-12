# Import library

The import library registers `.md`, `.markdown`, `.txt`, and `.pdf` files and
retains exact, immutable snapshots. Logical identity is a portable `source_id`;
filenames and hashes never decide whether two inputs are the same document.

- `import_file(path)` creates a new source and version, including for identical
  bytes or equal basenames.
- `import_file(path, {.source_id = id})` explicitly revises an active source of
  the same kind. Repeating current bytes returns its existing version. Changed
  bytes, including a return to historical bytes, create a new latest version.
- Global SHA-256 deduplication shares a read-only retained file, preserving each
  source/version ID, input filename, and separate extraction/page history.
- `import_files(paths)` commits each new source independently, continues after
  failures, and returns ordered `ImportFileResult` entries containing either an
  `ImportResult` or a classified `ImportFailure`. No committed success is hidden
  by a later per-file error.

The importer stages a copy before hashing, so the retained bytes and recorded
digest describe the same snapshot even if the input changes during import.
Byte lookup, revision selection, and registration run under `BEGIN IMMEDIATE`.
No original absolute input path is stored in project state. Shared retained
paths may belong to an earlier source/version's directory; callers must use
`stored_path`, not reconstruct a path from IDs. Extraction and vault citations
still use the selected version's own kind, filename, and IDs.

Existing projects require `kc init` to apply migration 003. See the full
[source identity contract](../../docs/SOURCE_IDENTITY.md) and its regression and
verification record.
