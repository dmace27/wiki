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
| `kc import` | `FILE...` | Register supported sources and retain immutable project-local copies. |
| `kc extract` | `SOURCE_ID [--force]` | Extract native text, render pages, and run local OCR when configured. |
| `kc review extraction` | `SOURCE_ID [--page N (--text TEXT | --text-file PATH)]` | Show page image paths, extracted text, and status together; optionally correct one page before compilation. `--page N` alone narrows inspection. |
| `kc compile` | `--concept TITLE [--source SOURCE_ID...]` | Select relevant pages and create one schema-validated pending proposal. |
| `kc proposal list` | `[--status STATUS]` | List proposals. |
| `kc proposal show` | `PROPOSAL_ID` | Show proposed Markdown, diff, citations, and source-page references. |
| `kc proposal approve` | `PROPOSAL_ID` | Approve a valid pending proposal; does not write the vault. |
| `kc proposal reject` | `PROPOSAL_ID [--reason TEXT]` | Reject a pending proposal; does not write the vault. |
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
kc proposal approve prp_01j...
kc apply prp_01j...
kc search "markov chains"
```

## Output and exit behavior

- Standard output is result data. Progress and diagnostics go to standard error.
- `--json` emits one object with `ok`, `command`, and either `result` or
  `error`; no progress text may appear on standard output.
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
pending --approve--> approved --apply--> applied
pending --reject------------------------> rejected
pending/approved --newer proposal-------> superseded
```

`kc apply` must reject every status except `approved`. `kc compile` must never
modify the vault.

`kc proposal approve` changes review state only. `kc apply` copies cited
immutable sources before exposing their Markdown links, backs up an existing
article, and replaces only content between the configured managed markers. It
publishes article/citation/audit/FTS state after the atomic file replacement;
if that state transaction fails, it restores the pre-apply article bytes.

Extraction correction changes only the latest `source_pages` text, hash, and
status. It is refused after a proposal has cited the page because proposal
payloads and normalized citation offsets are immutable. Proposal list/show are
read-only, while reject changes only proposal review metadata. None of these
review operations writes or edits a vault article.
