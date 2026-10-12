# Model adapters

The models library implements MVP step 1C: a local Ollama-compatible
`LanguageModel`, a schema-constrained concept prompt, semantic citation
validation, and redacted model-run auditing.

## Capability boundaries

`LanguageModel` receives only three in-memory values: a system prompt, a user
prompt, and a response schema. It returns untrusted text. The interface exposes
no project root, filesystem path, database, vault writer, command runner, or
write callback. Provider implementations therefore cannot write vault files.

`ModelRunner` is the surrounding application service. It:

1. reads the selected provider, base URL, model, and timeout from `kc.json`;
2. builds the versioned `concept-v1` prompt for the requested concept;
3. hashes the complete canonical request without persisting prompt/evidence;
4. invokes the provider;
5. strictly parses the `ArticleProposal` contract;
6. validates every citation against the exact supplied page text; and
7. writes one terminal `model_runs` audit row.

It deliberately does **not** insert into `proposals`. Part 2A owns creation of
a pending proposal after page selection and compiler-level validation.

## Ollama request

The adapter posts to `<base_url>/api/chat` with:

- `stream: false`, so one complete JSON envelope is validated;
- the complete proposal JSON Schema in `format`, compiled from the public schemas;
- the same schema in the prompt to ground smaller local models; and
- `temperature: 0` for deterministic structured output.

libcurl performs one bounded JSON POST. It does not use credentials, cookies,
redirects, environment proxies, or filesystem output. HTTP response bodies are capped
at 16 MiB and provider error bodies are never persisted.

## Validation and audit statuses

- `completed`: the JSON shape, requested concept target, operation, and every
  citation are valid. `response_json` contains canonical validated JSON.
- `invalid_response`: JSON parsing, strict proposal parsing, or citation
  validation failed. No proposal is returned. Untrusted output is replaced by
  a redacted record containing only its SHA-256, byte size, and issue count.
- `failed`: HTTP/provider execution failed. The database stores a fixed generic
  diagnostic and no response body.

Citation checks require the page ID to be among the supplied evidence,
`0 <= start_char < end_char <= page.text.size()`, and the normalized page
substring to equal `quote`. Offsets are byte offsets into the UTF-8 page text,
matching the current C++/SQLite contract.

Target title/slug validation follows the shared
[concept identity contract](../../docs/CONCEPT_IDENTITY.md). The compiler supplies
the stored slug for updates; new concepts use the canonical slug. The prompt
and validator require the exact resolved display title and expected slug.

Response parsing also enforces the shared
[Markdown text policy](../../docs/MARKDOWN_METADATA.md): safe one-line headings
and related destinations, valid Unicode, and no reserved ownership markers.
The prompt requests plain text; the writer escapes model markup and folds
multiline prose. Unsafe output never becomes an approvable proposal.

The [strict JSON contract](../../docs/JSON_CONTRACTS.md) rejects numeric
coercion and requires meaningful cited blocks in every section. Public schemas,
runtime shape validation, and the model schema share one source. Repeated
section keys are supported as ordered independent sections. Domain semantics
also apply to typed adapter output, review, approval, and application.
