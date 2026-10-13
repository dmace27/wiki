# Vault library

The vault library implements MVP step 2B and is the only project layer that
can write an article. `VaultWriter` accepts immutable structured proposals and
enforces the review boundary: `approve` changes proposal state only, while
`apply` accepts exactly the `approved` state.

An application:

- resolves every cited page to its immutable retained source and copies that
  source under the configured vault `_sources` directory;
- renders proposal blocks, `[[Related Concepts]]`, and page-linked footnotes;
- creates a new article or replaces only the configured `kc:managed` region;
- refuses malformed/missing markers and untracked file collisions unless the
  caller supplies the explicit overwrite option;
- backs up all previous bytes before an atomic temporary-file rename; and
- commits article identity, citations, apply audit, proposal state, and FTS
  content together only after the filesystem write succeeds.

Apply and restore publish a fully synced filesystem journal before backups,
source copies or article staging. Native atomic exchange retains and verifies
exact displaced user bytes; new files use exclusive publication. A SQLite commit
marker records article/citation/proposal/audit/snapshot/FTS publication together.
Caught failures and process death use the same restart recovery; conflicting
external edits remain untouched and require explicit review. Existing source
copies and committed backups remain history. See the
[durability and recovery protocol](../../docs/APPLY_RECOVERY.md) for supported
systems, failure behavior, reconciliation and tests.

Approval and apply share the [concept identity rules](../../docs/CONCEPT_IDENTITY.md).
Updates retain stored title/slugs/paths, including legacy case-only payloads.
Neither the overwrite flag nor a new concept can claim another tracked article's
destination; portable Unicode/case sibling collisions also fail before writing.

The [Markdown metadata contract](../../docs/MARKDOWN_METADATA.md) is domain-neutral:
new articles omit inferred tags and quote every YAML string. Aliases become
user-owned after creation. Updates preserve frontmatter bytes and parse final
aliases for FTS instead of indexing ignored proposal suggestions. Shared domain
validation rejects unsafe headings, relationships, Unicode, and ownership
markers before review/approval. Model prose is escaped literal text; only the
writer creates links/footnotes. Final metadata and ownership checks run before
source copies, backups, or article writes.

## Accumulated updates

Updates deterministically retain approved blocks, relationships, and historical
citations before adding new source-grounded content. `--source` filters new
model evidence; `--regenerate` deliberately proposes complete replacement and
review lists all removed and added claims. A compiler-owned base apply-run ID
and managed-region digest guard publication, approval, and apply. Manual notes
belong outside the markers; managed edits require restoration and fresh review.
See [update semantics, revision checks, and legacy recovery](../../docs/ACCUMULATED_KNOWLEDGE.md)
for the exact reconciliation and review JSON contracts.

## Task 8 review contract

Preview and apply use the same pure renderer. Approval binds exact before/after
bytes, metadata, source evidence, destination, and collision consent with the
inspected SHA-256 token. Any changed rendering requires renewed approval.
Referenced extraction pages are retained; corrections/forced retries publish
new current identities rather than rewriting citation history.
See [exact previews and provenance-preserving review](../../docs/REVIEW_EXPERIENCE.md).

## Task 9 history and restore

`history` lists applied snapshots and restore audit records. `restore` first
shows exact historical Markdown and a diff against current user bytes; executing
it requires that preview token. It restores content, citations, aliases/FTS,
and the active structured revision in one journaled operation. Older applied
proposals remain immutable history. Updates load the active pointer so a restore
cannot leave accumulation based on a later, inactive apply. Migration 007 adds
commit markers, full snapshots, the active pointer, and append-only restore audit.
