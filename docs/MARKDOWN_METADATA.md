# Markdown metadata and rendering

Task 4 policy, October 12, 2026. Applies to every subject; no topic taxonomy or
inferred tags are part of the proposal contract. New articles omit `tags`.

## Frontmatter and alias ownership

The writer creates a YAML mapping containing `kc_schema: 1`, `article_id`,
`title`, and `aliases`. Every emitted string is double quoted using JSON string
escaping, which is compatible with YAML. Unicode is retained; quotes and
backslashes are escaped. Titles and aliases such as `true`, `null`, `123`,
`1e3`, `yes`, and dates therefore remain strings. No aliases renders as
`aliases: []`, never a null field.

`kc_schema`, `article_id`, and `title` identify the tracked article and belong
to the compiler. Changing them manually makes an update fail before any source
copy, backup, article replacement, audit write, or index change. Restore the
recorded identity to apply an update; changing a title is not a supported rename
operation. Canonical identity handling remains as documented in
[Concept identity](CONCEPT_IDENTITY.md).

Proposal aliases seed new articles. After creation, aliases, tags, and custom
frontmatter belong to the user. Updates preserve all frontmatter bytes and
ignore aliases suggested by the update proposal. This is an explicit ownership
rule, also included in the model prompt. Edit aliases in Obsidian or the Markdown
file to change them. At each successful apply, the writer parses the completed
article with yaml-cpp and indexes its effective aliases, rather than the update
proposal's suggestions. Thus Obsidian and the CLI see the same alias list.
Manual changes between applications are not automatically reindexed.

Aliases must be a sequence of nonempty, single-line YAML strings. Ordinary
legacy plain aliases such as `Markov chain` remain accepted. Quote values that
could be YAML booleans, nulls, numbers, or dates; explicitly typed `!!str` values
are also supported. Legacy missing/null `aliases` is interpreted as an empty
list and preserved byte-for-byte. Existing tags, including the former generated
`probability` tag, remain user-owned; remove incorrect old tags manually.

Invalid YAML, duplicate mapping keys, YAML merge keys, nested aliases,
ambiguous scalar types, or changed compiler identity make apply fail without
changing article/index state. This avoids guessing which value a metadata
reader will use. An existing tracked article needs its original frontmatter;
missing frontmatter is an error, not an invitation to regenerate user metadata.

## Generated content policy

Proposal text is **plain UTF-8 text**. The writer provides headings, paragraph
and bullet structure, related-page links, footnote references, and source links.
ASCII punctuation in model text and source display labels is backslash escaped
as literal Markdown text. Model Markdown links, wiki links, footnotes, raw HTML,
autolinks, images, embeds, emphasis, math delimiters, and code fences display as
literal text. They do not create active markup or remote resource requests.

Multiline paragraph/bullet text and relationship reasons are folded into one
inline text run by trimming and collapsing whitespace. A continuation cannot
create a heading, list, code fence, HTML block, or footnote definition, and the
writer's citation stays attached to its entire block. Preserved line breaks,
formatted math, and model-authored code blocks are not supported by this text
contract. User-authored Markdown outside the managed region retains its normal
formatting.

Headings and aliases must contain visible text on one line. Related titles
must also be portable page filenames, with no `[]#|^` wiki-link delimiters or
heading/block selectors. A relationship that needs such a destination must be
curated to an unambiguous page name; the writer refuses to silently link a
different target. Unicode titles/aliases remain supported. Invalid UTF-8,
embedded NUL, control characters, invisible format characters, and the reserved
`kc:managed:` namespace are rejected in every proposal text field, including
citation quotes. Section IDs are 1–128 ASCII letters, digits, dots, underscores,
or hyphens so they cannot inject comment attributes.

These semantic checks live in the domain layer. JSON parsing, model response
validation, compiler persistence, review, approval, and apply all use them.
Unrenderable generated output remains an `invalid_response` model run and never
becomes a pending proposal. Unsafe stored/legacy proposals cannot bypass checks
through approval or an already-approved status.

## Source links and final article checks

Only the writer constructs source references, from validated citation rows and
immutable source metadata. Labels are escaped and multiline display names are
folded. Link targets percent-encode UTF-8 bytes and all characters outside ASCII
letters/digits and `/ - _ . ~`. Spaces, parentheses, brackets, percent signs,
fragments, and non-ASCII path characters therefore cannot break a target or
supply a second link. PDF page fragments are appended by the writer.

Before source copies or other side effects, apply parses final frontmatter,
checks that there are exactly the two configured ownership markers, verifies
that the managed region equals the safe rendering, and refuses user footnote
definitions that shadow generated page references. Unrelated user additions
remain byte-identical. No database migration or automatic rewrite of old
articles is required.

## Verification

The deterministic regression suite covers actual import/extract/compile →
approve/apply → search create/update journeys for biology (`Photosynthesis`),
history (`Silk Road`), and programming (`Recursion`). It parses resulting YAML,
checks Unicode aliases, preserves custom user metadata, and proves that removed
seed aliases and ignored model suggestions are absent from search.

Writer tests parse scalar-looking titles and aliases with yaml-cpp and verify
quoted string tags, empty lists, and compatibility with legacy null aliases.
They also cover invalid user YAML, early heading/relationship/marker rejection,
malicious source labels, Unicode/space/parenthesis URL escaping, multiline
markup, forged footnotes, and extra ownership markers. Failed applications
preserve article bytes, backup state, audits, and FTS rows.

Build/test commands (with `VCPKG_ROOT` set as in the root README):

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

The YAML dependency is `yaml-cpp 0.9.0#1` at the existing pinned vcpkg baseline.
Results below are local macOS arm64 deterministic checks. They do not claim a
real-model quality result or manual Obsidian rendering/navigation verification.

Final results: development **95/95 passed**; ASan/UBSan with warnings-as-errors
**95/95 passed**; release build passed. Release CLI initialization/idempotency
and proposal approval/apply error-routing smoke checks also passed. The latter
are CLI routing checks; successful article rendering is exercised by the real
library workflow with a deterministic model double in the regression suite.
