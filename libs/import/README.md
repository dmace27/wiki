# Import library

The import library implements MVP step 1A:

- registers `.md`, `.markdown`, `.txt`, and `.pdf` sources;
- computes streaming SHA-256 hashes over the exact imported bytes;
- deduplicates hashes already present in project state;
- appends a `source_versions` row when a known filename has changed; and
- retains each new version at a read-only, project-relative path under
  `sources/<source_id>/<source_version_id>/`.

The importer stages a copy before hashing, so the retained bytes and recorded
digest describe the same snapshot even if the input file changes during an
import. Database registration is transactional and no original absolute path
is stored.
