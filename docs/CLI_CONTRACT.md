# CLI Contract

The executable name is `kc` (Knowledge Compiler). Commands use human-readable
output by default and support `--json` for scripts and the future local UI.

Every command accepts:

```text
--project PATH     Project root; defaults to current directory or nearest parent with kc.json
--json             Emit one JSON document to stdout
--quiet            Suppress progress output
```

## Commands

| Command | Arguments | Behavior |
| --- | --- | --- |
| `kc init` | `--vault PATH` | Create config, state directory, database, and initial migration. |
| `kc doctor` | none | Check config, writable paths, SQLite, Poppler, OCR, and configured local model. |
| `kc status` | none | Show source, page, proposal, and article counts plus pending work. |
| `kc import` | `FILE... [--source SOURCE_ID]` | Create new logical sources, or explicitly revise one source with exactly one file; retain immutable bytes. |
| `kc extract` | `SOURCE_ID [--force]` | Extract native text, render pages, and run local OCR when configured. |
| `kc review extraction` | `SOURCE_ID [--page N] [--text TEXT | --text-file PATH | --accept] [--open-image]` | Inspect current image/text/status; accept unchanged or correct text. Cited corrections retain old identities. Mutations and image opening require `--page`. |
| `kc compile` | `--concept TITLE [--source SOURCE_ID...] [--regenerate]` | Select new evidence and create a validated pending proposal; updates accumulate by default. `--regenerate` explicitly proposes replacement of an existing article. |
| `kc proposal list` | `[--status STATUS]` | List proposals. |
| `kc proposal show` | `PROPOSAL_ID [--allow-overwrite-user-file]` | Show exact Markdown/diff, destination/collisions/validation, citations, and preview token. |
| `kc proposal preview` | `PROPOSAL_ID [--allow-overwrite-user-file]` | Focused read-only rendering/diff, including invalid stored-state diagnostics. |
| `kc proposal approve` | `PROPOSAL_ID --preview SHA256 [--allow-overwrite-user-file]` | Approve the exact inspected valid rendering; no vault write. |
| `kc proposal revoke` | `PROPOSAL_ID` | Return an unused approval to pending; no vault write. |
| `kc proposal reject` | `PROPOSAL_ID [--reason TEXT]` | Reject pending or unused approved work; no vault write. |
| `kc apply` | `PROPOSAL_ID [--allow-overwrite-user-file]` | Atomically write an approved proposal and update citations/search. The flag is required to replace an untracked colliding file. |
| `kc search` | `QUERY [--limit N]` | Search article title, aliases, and body using local FTS5. |
| `kc open` | `ARTICLE_ID` | Deferred platform integration to open a vault article in Obsidian. |
| `kc serve` | `[--port PORT]` | Deferred loopback review/search UI server. |

## MVP workflow

```bash
kc init --vault ./vault
kc import "./Probability Notes Week 8.pdf"
kc extract src_01j...
kc review extraction src_01j...
kc review extraction src_01j... --page 3 --text-file corrected-page.txt
kc compile --concept "Markov Chains"
kc proposal show prp_01j...
kc proposal approve prp_01j... --preview PREVIEW_SHA256
kc apply prp_01j...
kc search "markov chains"

# The same workflow applies without code/config changes in other domains.
kc compile --concept "Cellular Respiration" --source src_01j...
kc compile --concept "The French Revolution"
```

## Concept identity

Compile normalizes horizontal whitespace and Unicode NFC, resolves concepts
with full locale-independent case folding, and preserves an applied page's
display title, slug, and filename. Approval/apply and exact search use those
same rules. Distinct titles sharing a slug (e.g. `A-B` and `A B`) and unsafe
portable filenames fail with `invalid_concept` before calling the model.
Non-English word/phrase matching supports Unicode case and normalization;
unsegmented text, inflection, and implicit concepts may require `--source`.
See [concept identity](CONCEPT_IDENTITY.md) for precise rules and legacy behavior.

## Source imports and revisions

`kc import FILE...` creates a distinct logical source for **every input**.
Use `kc import --source SOURCE_ID FILE` to revise one active source, including
a renamed/moved file. The source kind must match; display metadata stays stable.
Repeating current bytes is idempotent. Returning to historical bytes creates a
new latest version. Identical bytes share retained storage across identities.
Run `kc init` to upgrade existing projects before using these rules.

Successful JSON import entries include `outcome` (`new_source`, `new_revision`,
or `unchanged`), `source_created`, `version_created`, `deduplicated` (retained
bytes reused), and `restored` (historical bytes activated in a new revision).
`deduplicated` can be true while `version_created` is true.

A batch processes every file in argument order and commits each independently.
Its JSON envelope always includes `result.sources` (successes), `result.files`
(ordered input paths with `ok` and either `result` or `error`), and
`result.partial_success`. If any file fails, top-level `ok` is false and an
`error` accompanies the durable successes: `partial_import` for mixed results,
`import_failed` if all failed. The exit code is the highest per-file failure
code: 6 for state/internal failures, 3 for unavailable files, 2 for invalid
inputs. Human output prints successful identities to stdout and failures to
stderr, including when `--quiet` suppresses successful output.

[Source identity rules](SOURCE_IDENTITY.md) document renames, revisions,
activation, shared files, and complete batch examples.

## Output and exit behavior

- Standard output is result data. Progress and diagnostics go to standard error.
- `--json` emits one object with `ok`, `command`, and either `result` or
  `error`; no progress text may appear on standard output. Failed import batches
  include both `result` and `error` to retain per-file outcomes. Invalid proposal
  previews also retain their inspectable result alongside the error.
- Exit `0`: completed successfully.
- Exit `2`: invalid command arguments or invalid project configuration.
- Exit `3`: a requested resource does not exist or cannot be used in its current
  state, such as applying a rejected proposal.
- Exit `4`: an external adapter failed, such as OCR, PDF extraction, or model
  service.
- Exit `5`: validation failed; no vault write occurs.
- Exit `6`: an unexpected internal error; write a redacted diagnostic log.

## State transitions

```text
pending --approve exact preview--> approved --apply--> applied
approved --revoke----------------> pending
pending/approved --reject--------> rejected
pending/approved --newer proposal-------> superseded
```

`kc apply` must reject every status except `approved`. `kc compile` must never
modify the vault.

`kc proposal approve` changes review state only. `kc apply` copies cited
immutable sources before exposing their Markdown links, backs up an existing
article, and replaces only content between the configured managed markers. It
publishes article/citation/audit/FTS state after the atomic file replacement;
if that state transaction fails, it restores the pre-apply article bytes.

Extraction correction targets current pages of the latest source version.
Referenced text/images remain immutable: corrections and forced retries create
new current page identities linked to retained historical pages. `--accept`
records inspected text unchanged. `--open-image` launches the verified selected
image with direct argv while showing extracted text/status in the terminal.
Proposal show/preview/list are read-only; approval, revoke, and reject change
only review state. None writes a vault article. Approval/apply independently
rebuild and match the preview token; changed bytes/metadata/configuration or
consent require renewed review. Invalid `proposal preview --json` returns exit
5 with both an error and inspectable `result.preview` data. See the precise
[preview JSON, revision, and review workflow](REVIEW_EXPERIENCE.md).

## Article metadata and generated text

New articles omit inferred tags and use quoted YAML strings with `aliases: []`
when empty. Proposal aliases seed creation; subsequent frontmatter aliases are
user-owned, preserved on update, and read from the completed file for indexing.
Update proposal aliases are suggestions with no effect on existing metadata.
Invalid YAML/ambiguous aliases or changed compiler identity abort apply before
writing. Model text renders literally; the writer supplies links and citations.
Headings, relationships, and reserved markers are validated before proposals
enter review. See [Markdown metadata and rendering](MARKDOWN_METADATA.md).

## Accumulated updates

Updates deterministically retain approved blocks, relationships, and historical
citations before adding new source-grounded content. `--source` filters new
model evidence; `--regenerate` deliberately proposes complete replacement and
review lists all removed and added claims. A compiler-owned base apply-run ID
and managed-region digest guard publication, approval, and apply. Manual notes
belong outside the markers; managed edits require restoration and fresh review.
See [update semantics, revision checks, and legacy recovery](ACCUMULATED_KNOWLEDGE.md)
for the exact reconciliation and review JSON contracts.
