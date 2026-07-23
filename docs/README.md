# Knowledge Compiler Technical Documentation

These documents are the implementation contract for the Personal Probability
Compiler MVP. Agents should preserve these contracts unless they update the
relevant document and explain the migration.

| Document | Use it for |
| --- | --- |
| [Architecture](ARCHITECTURE.md) | System boundaries, stack, project layout, and safety rules. |
| [Implementation plan](MVP_IMPLEMENTATION_PLAN.md) | Task order, ownership, dependencies, and acceptance criteria. |
| [CLI contract](CLI_CONTRACT.md) | Command names, arguments, output conventions, and MVP workflow. |
| [Data contracts](DATA_CONTRACTS.md) | IDs, JSON schemas, citation validation, and Markdown rendering format. |
| [State database](STATE_DATABASE.md) | SQLite design, migration rules, and initial DDL. |

## Non-negotiable MVP rules

1. Processing is local by default. API providers are optional adapters and must
   never receive data unless explicitly configured.
2. Every generated content block must be backed by one or more page-level
   citations to an immutable source version.
3. A model produces structured proposals, never arbitrary filesystem edits.
4. The user approves a proposal before the compiler writes the Obsidian vault.
5. User-authored Markdown is never overwritten. The compiler only owns content
   between its managed-section markers.
6. The first end-to-end target is one useful, source-linked `Markov Chains.md`
   article produced from real probability notes.
