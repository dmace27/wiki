# Strict JSON contracts

Task 5 policy, October 12, 2026. The four supported JSON contracts are project
configuration, extracted pages, citations, and article proposals.

## Authoritative schemas and validation boundaries

The files under [`schemas/`](../schemas/) are the structural source of truth.
CMake compiles their contents into the domain library and watches them for
changes. Installed executables need no developer source path to read them.
`contract_schema()` exposes these same schemas. The model prompt uses the
article schema with its citation reference embedded under `$defs`, without
maintaining a separate handwritten prompt schema.

All `parse_*` functions and nlohmann `from_json` conversion hooks perform the
same sequence: check numeric primitives and bounds, validate schema shape,
convert checked values, and run shared domain semantics. Calling
`json.get<ArticleProposal>()` cannot bypass the semantic checks. Invalid parses
return no value; conversion hooks throw. Unknown properties remain forbidden at
every object level, including nested citations.

Runtime schema validation uses the existing pinned
`json-schema-validator 2.4.0` dependency. The public schemas retain Draft 2020-12
identifiers and use the keyword subset exercised by the regression suite. A
schema-only check is not evidence validation: relative path safety, portable
filenames, Unicode/ownership rules, and comparisons between arbitrary fields
also require domain semantics. Exact citation spans and request target identity
require the evidence/request-aware model validator. Review, approval, and apply
reparse stored proposals through the same domain boundary.

## Integer representations and bounds

Integer fields accept nonnegative signed or unsigned **JSON integer tokens**.
The runtime rejects booleans, null, strings, arrays, objects, negative values,
fractions, overflow, and floating representations such as `1.0`, `1e0`, and
`-0.0`. Checking happens before narrowing to `uint32_t` or `size_t`, and before
the schema dependency can compare an oversized unsigned number as a signed
integer. Huge wire numbers that nlohmann parses as floats are rejected too.
Integer `-0` represents zero and is accepted only where zero is allowed.

| Field | Inclusive accepted numeric range |
| --- | --- |
| Configuration/proposal `schema_version` | Exactly `1` |
| Ollama `timeout_seconds` | `1`–`2147483647` |
| Extracted `page_number` | `1`–`4294967295` |
| Citation `start_char` | `0`–`9223372036854775806` |
| Citation `end_char` | `1`–`9223372036854775807` |

Timeouts use the portable signed 32-bit curl `long` limit. Page numbers fit
`uint32_t`. Citation offsets fit signed SQLite storage and the native `size_t`;
the advertised macOS arm64 target has a 64-bit `size_t`. The start maximum
leaves room for a strictly greater end. Domain validation also requires
`start_char < end_char`; evidence validation requires `end_char <= page.text.size()`.
These representation limits are not corpus/context budgets or evidence-quality
claims.

JSON Schema's mathematical definition of an integer can let other validators
accept an integral decimal/exponent representation. Consumers must additionally
enforce the integer-token policy above. The pinned schema validator and runtime
are tested against the same token fixtures; model output is always checked by
the runtime even if a remote server's schema engine accepts `1.0`.

## IDs and meaningful articles

IDs require the exact object prefix followed by 26 uppercase Crockford Base32
characters. The first ULID character must be `0`–`7`, since its 130-bit encoding
represents 128 bits. Lowercase, ambiguous letters (`I`, `L`, `O`, `U`), incorrect
lengths, prefix-only values, and overflowing leading characters are rejected.
The same domain ID helper is used by typed values and model requests, and all
public/model schemas enforce the matching pattern and length.

Every article must contain at least one section, and every section must contain
at least one cited block of visible prose. Whitespace-only ASCII or Unicode
text, empty block arrays, and uncited content are invalid. Aliases and headings
remain visible single-line text under the
[Markdown metadata policy](MARKDOWN_METADATA.md). Empty related-concept lists
remain supported; relationship reasons may be empty, since the cited title can
stand alone.

Repeated section keys are intentional: a proposal may have multiple `example`
sections or separate working explanations. Occurrences remain in proposal
order and each must have its own cited content. Normalized citation indexes
continue across occurrences of the same key; no later occurrence silently
replaces an earlier one. Exact duplicate prose is not automatically deduplicated.
An empty occurrence cannot hide behind a populated section with the same key.

A `create_article` proposal must omit `article_id`; an `update_article` proposal
must include a valid article ID. Shared semantics also reject unsupported C++
enum values so an injected typed model adapter cannot bypass JSON validation.
Request-specific title/slug/article-ID agreement remains an additional model
and stored-identity check.

## Verification

[`tests/fixtures/contracts/cases.json`](../tests/fixtures/contracts/cases.json)
contains **331 shared cases**, expressed as patches to the four valid fixtures.
Each records expected schema validity and full runtime validity separately for
cross-field/domain checks that standard JSON Schema cannot express.

Independent disk-schema validation, compiled shape validation, normal parsing,
and direct nlohmann conversion run the same cases. They cover numeric endpoints
and one-past boundaries, primitive/shape mismatches, every required-property
omission, extra properties at every object level, ID lengths/alphabet/overflow,
ASCII/Unicode whitespace, operation/article-ID rules, and repeated section keys.
Additional tests cover decimal/exponent wire syntax and typed adapter values.

All invalid article cases also run through model generation, compiler
publication, review, approval, and apply. They remain redacted invalid model
runs, produce no pending proposals through the compiler, cannot be approved,
and cannot create an article even if an invalid stored proposal is manually
marked approved. A positive repeated-section journey verifies both Markdown
and normalized citation rows.

The black-box `kc_cli_json_contract` test reproduces repeated `kc init` with
invalid numeric configuration. Failure preserves the user configuration bytes
and database digest; restoring valid configuration permits initialization again.

Reproduce with `VCPKG_ROOT` set as in the root README:

```sh
cmake --preset dev
cmake --build --preset dev -j 4
ctest --preset dev
cmake --preset sanitized
cmake --build --preset sanitized -j 4
ctest --preset sanitized
cmake --preset release
cmake --build --preset release -j 4
```

No schema migration or automatic repair of invalid old JSON is performed.
Correct invalid configuration explicitly. Invalid old proposals require a new,
valid proposal; their retained evidence and audit history are not rewritten.

Final local macOS arm64 results: development **105/105 passed**;
ASan/UBSan with warnings-as-errors **105/105 passed**; release build passed.
Release CLI initialization/idempotency and the numeric-configuration rejection
regression both passed. These are deterministic contract checks, not a real-model
quality or interactive Obsidian evaluation.
