# Data Contracts

## General conventions

- IDs are opaque ULIDs with a type prefix: `prj_`, `src_`, `ver_`, `pg_`, `art_`,
  `prp_`, and `run_`.
- Timestamps are UTC RFC 3339 strings.
- Content hashes use lowercase SHA-256 hex.
- Text is UTF-8.
- JSON uses `snake_case` keys and JSON Schema Draft 2020-12.
- A source version is immutable. Citations use `page_id`, which belongs to a
  specific immutable source version.

## Project configuration format

```json
{
  "schema_version": 1,
  "project_id": "prj_01J00000000000000000000000",
  "paths": {
    "sources": "sources",
    "vault": "vault",
    "state": ".knowledge-compiler/state.sqlite",
    "cache": ".knowledge-compiler"
  },
  "vault": {
    "source_directory": "_sources",
    "generated_section_id": "knowledge-compiler"
  },
  "providers": {
    "llm": {
      "default": "ollama",
      "ollama": {
        "base_url": "http://127.0.0.1:11434",
        "model": "gemma3:12b",
        "timeout_seconds": 300
      }
    },
    "ocr": {
      "default": "tesseract",
      "language": "eng"
    }
  }
}
```

`api_key_env` is not part of the current configuration schema. Unknown keys
are rejected; provider extensions require an explicit contract change.

## Extracted page schema

The authoritative schema is [extracted-page.schema.json](../schemas/extracted-page.schema.json).
See [Strict JSON contracts](JSON_CONTRACTS.md) for numeric bounds, exact IDs,
structural validation, and shared semantic checks.

Markdown and TXT sources create one logical page. Each PDF page creates exactly
one page record, even when text or OCR extraction fails.

## Citation schema

The authoritative schema is [citation.schema.json](../schemas/citation.schema.json).
See [Strict JSON contracts](JSON_CONTRACTS.md) for numeric bounds, exact IDs,
structural validation, and shared semantic checks.

Validation requirements:

1. `page_id` must exist.
2. `0 <= start_char < end_char <= page.text.length`.
3. The whitespace-normalized page substring and `quote` must match.
4. Each generated block must contain at least one valid citation.

The local model boundary treats offsets as byte offsets into the UTF-8 string
stored in SQLite. A model response is `invalid_response` when its JSON shape is
invalid, a citation references a page outside the supplied evidence, an offset
is out of bounds, or its whitespace-normalized quote does not match the page
substring. Invalid output never becomes a proposal.

## Article proposal schema

The LLM returns this proposal. It cannot request paths, Markdown, commands, or
arbitrary operations.

The authoritative schema is [article-proposal.schema.json](../schemas/article-proposal.schema.json).
See [Strict JSON contracts](JSON_CONTRACTS.md) for numeric bounds, exact IDs,
structural validation, and shared semantic checks.

## Markdown format

The compiler owns only the content between `kc:managed` markers. The writer
builds source footnotes itself from validated citation records. Text fields
are rendered literally, with multiline prose folded into one cited block. See
[Markdown metadata and rendering](MARKDOWN_METADATA.md) for the content policy,
YAML string serialization, alias ownership, and final article checks.

Vault source copies use the immutable name
`<source_id>-<source_version_id>.<extension>` under the configured
`vault.source_directory`. PDF footnotes append `#page=<page_number>`; this
keeps a citation target stable even after a later version of the same logical
source is imported.

```markdown
---
kc_schema: 1
article_id: "art_01J00000000000000000000000"
title: "Markov Chains"
aliases:
  - "Markov chain"
---

# Markov Chains

<!-- kc:managed:start id="knowledge-compiler" -->

## My working explanation

A Markov chain moves between states\. [^pg_01J00000000000000000000000]

## Key ideas

- State\: the current condition of the process\. [^pg_01J00000000000000000000000]
- Transition probability\: the chance of moving between states\. [^pg_01J00000000000000000000001]

## Related concepts

- [[Conditional Probability]] — Transitions use conditional probabilities\. [^pg_01J00000000000000000000000]

## Sources

[^pg_01J00000000000000000000000]: [Probability Notes, Week 8, p. 3](_sources/src_01J00000000000000000000000-ver_01J00000000000000000000000.pdf#page=3)
[^pg_01J00000000000000000000001]: [Probability Notes, Week 8, p. 4](_sources/src_01J00000000000000000000000-ver_01J00000000000000000000000.pdf#page=4)

<!-- kc:managed:end -->

## My additions

User-authored notes live here and are never overwritten.
```
