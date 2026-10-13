# Knowledge Compiler Technical Documentation

These documents are the implementation contract for the subject-neutral
Knowledge Compiler MVP. Agents should preserve these contracts unless they update the
relevant document and explain the migration.

| Document | Use it for |
| --- | --- |
| [Architecture](ARCHITECTURE.md) | System boundaries, stack, project layout, and safety rules. |
| [Development baseline](DEVELOPMENT_BASELINE.md) | Persistent dependency bootstrap, selected target/tool versions, and clean preset verification. |
| [Implementation plan](MVP_IMPLEMENTATION_PLAN.md) | Task order, ownership, dependencies, and acceptance criteria. |
| [CLI contract](CLI_CONTRACT.md) | Command names, arguments, output conventions, and MVP workflow. |
| [Strict JSON contracts](JSON_CONTRACTS.md) | Integer-token bounds, authoritative schemas, meaningful sections, and Task 5 verification. |
| [Data contracts](DATA_CONTRACTS.md) | IDs, JSON schemas, citation validation, and Markdown rendering format. |
| [Source identity](SOURCE_IDENTITY.md) | New imports, explicit revisions, byte reuse, partial batches, migration, and Task 2 verification. |
| [Markdown metadata](MARKDOWN_METADATA.md) | Domain-neutral YAML, user-owned aliases, literal content policy, and Task 4 verification. |
| [Concept identity](CONCEPT_IDENTITY.md) | Canonical titles, Unicode matching, slug collisions, portable filenames, and Task 3 verification. |
| [Extraction integrity](EXTRACTION_INTEGRITY.md) | Canonical path ownership, retained evidence hashes, UTF-8, and Task 6 race verification. |
| [Accumulated knowledge](ACCUMULATED_KNOWLEDGE.md) | Additive updates, explicit regeneration, base revisions, and Task 7 verification. |
| [Review experience](REVIEW_EXPERIENCE.md) | Exact rendered previews, approval tokens, OCR acceptance/opening, and Task 8 verification. |
| [Apply recovery](APPLY_RECOVERY.md) | Durable journals, crash reconciliation, external editor protection, history/restore, and Task 9 verification. |
| [State database](STATE_DATABASE.md) | SQLite design, migration rules, and initial DDL. |
| [Task 3 verification](TASK_3_VERIFICATION.md) | Automated end-to-end evidence and manual OCR/Obsidian test record. |

## Non-negotiable MVP rules

1. Processing is local by default. API providers are optional adapters and must
   never receive data unless explicitly configured.
2. Every generated content block must be backed by one or more page-level
   citations to an immutable source version.
3. A model produces structured proposals, never arbitrary filesystem edits.
4. The user approves a proposal before the compiler writes the Obsidian vault.
5. User-authored Markdown is never overwritten. The compiler only owns content
   between its managed-section markers.
6. Each end-to-end target is one useful, source-linked concept article produced
   from real notes in any subject.
