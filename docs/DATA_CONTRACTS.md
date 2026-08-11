# Data Contracts

## General conventions

- IDs are opaque ULIDs with a type prefix: `src_`, `ver_`, `pg_`, `art_`,
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
  "project_id": "prj_01j...",
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

API configuration may contain an environment-variable name such as
`"api_key_env": "KNOWLEDGE_COMPILER_API_KEY"`, but never a credential value.

## Extracted page schema

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "ExtractedPage",
  "type": "object",
  "required": [
    "page_id",
    "source_version_id",
    "page_number",
    "text",
    "text_status"
  ],
  "properties": {
    "page_id": {
      "type": "string",
      "pattern": "^pg_[0-9A-HJKMNP-TV-Z]+$"
    },
    "source_version_id": {
      "type": "string",
      "pattern": "^ver_[0-9A-HJKMNP-TV-Z]+$"
    },
    "page_number": { "type": "integer", "minimum": 1 },
    "image_path": { "type": "string" },
    "text": { "type": "string" },
    "text_status": {
      "enum": ["native", "ocr_unreviewed", "reviewed", "failed"]
    },
    "language": { "type": "string", "default": "en" }
  },
  "additionalProperties": false
}
```

Markdown and TXT sources create one logical page. Each PDF page creates exactly
one page record, even when text or OCR extraction fails.

## Citation schema

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "Citation",
  "type": "object",
  "required": ["page_id", "start_char", "end_char", "quote"],
  "properties": {
    "page_id": { "type": "string", "pattern": "^pg_" },
    "start_char": { "type": "integer", "minimum": 0 },
    "end_char": { "type": "integer", "minimum": 1 },
    "quote": { "type": "string", "minLength": 1 }
  },
  "additionalProperties": false
}
```

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

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "ArticleProposal",
  "type": "object",
  "required": [
    "schema_version",
    "operation",
    "article",
    "sections",
    "related_concepts"
  ],
  "properties": {
    "schema_version": { "const": 1 },
    "operation": { "enum": ["create_article", "update_article"] },
    "article": {
      "type": "object",
      "required": ["title", "slug"],
      "properties": {
        "article_id": { "type": "string", "pattern": "^art_" },
        "title": { "type": "string", "minLength": 1 },
        "slug": {
          "type": "string",
          "pattern": "^[a-z0-9]+(?:-[a-z0-9]+)*$"
        },
        "aliases": {
          "type": "array",
          "items": { "type": "string" },
          "uniqueItems": true
        }
      },
      "additionalProperties": false
    },
    "sections": {
      "type": "array",
      "minItems": 1,
      "items": {
        "type": "object",
        "required": ["key", "heading", "blocks"],
        "properties": {
          "key": {
            "enum": [
              "working_explanation",
              "key_ideas",
              "example",
              "related_concepts"
            ]
          },
          "heading": { "type": "string" },
          "blocks": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["kind", "text", "citations"],
              "properties": {
                "kind": { "enum": ["paragraph", "bullet"] },
                "text": { "type": "string", "minLength": 1 },
                "citations": {
                  "type": "array",
                  "minItems": 1,
                  "items": { "$ref": "citation.schema.json" }
                }
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    },
    "related_concepts": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["title", "reason", "citations"],
        "properties": {
          "title": { "type": "string" },
          "reason": { "type": "string" },
          "citations": {
            "type": "array",
            "minItems": 1,
            "items": { "$ref": "citation.schema.json" }
          }
        },
        "additionalProperties": false
      }
    }
  },
  "additionalProperties": false
}
```

## Markdown format

The compiler owns only the content between `kc:managed` markers. The writer
builds source footnotes itself from validated citation records.

Vault source copies use the immutable name
`<source_id>-<source_version_id>.<extension>` under the configured
`vault.source_directory`. PDF footnotes append `#page=<page_number>`; this
keeps a citation target stable even after a later version of the same logical
source is imported.

```markdown
---
kc_schema: 1
article_id: art_01j...
title: Markov Chains
aliases:
  - Markov chain
tags:
  - probability
---

# Markov Chains

<!-- kc:managed:start id="knowledge-compiler" -->

## My working explanation

A Markov chain describes a process that moves between states, where the next
state depends on the current state rather than the full earlier history.
[^pg_01j_a]

## Key ideas

- **State:** the current condition of the process. [^pg_01j_a]
- **Transition probability:** the chance of moving between states. [^pg_01j_b]

## Related concepts

- [[Conditional Probability]]
- [[Random Variables]]
- [[Random Walks]]

## Sources

[^pg_01j_a]: [Probability Notes, Week 8, p. 3](_sources/src_01j...pdf#page=3)
[^pg_01j_b]: [Probability Notes, Week 8, p. 4](_sources/src_01j...pdf#page=4)

<!-- kc:managed:end -->

## My additions

User-authored notes live here and are never overwritten.
```
