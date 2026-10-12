# Finish plan: Knowledge Compiler

Reviewed: October 12, 2026. Repository revision: `3bdb35d` (`generalized for all topics`).

## Assessment

The project has a working, modular CLI core, but it is not ready for a public release. Import, extraction, generic concept selection, Ollama proposals, citation validation, approval, managed Markdown writes, backups, and FTS search are implemented. The remaining work is primarily correctness, trustworthy updates, usable review, real-runtime verification, and distribution—not rebuilding the pipeline.

The largest product risk is that **updating a concept currently regenerates its managed content without giving the model the previously approved article**. A later compilation using a restricted source set can replace earlier knowledge and citations. That falls short of the accumulated personal reference library described in `PLANNING.md`.

There are also concrete bugs: unrelated inputs with the same filename become revisions of one source; case-only concept updates cannot be applied; every new article is tagged `probability`; numeric JSON values are coerced instead of strictly validated; and installed migration discovery depends on how the executable is invoked.

Treat `MVP.md` and the current technical docs as the first-release scope. The older planning conversation supplies product intent, but its API providers, topic maps, automatic discovery, semantic search, and companion UI are explicitly deferred by the newer MVP. A shippable first version can be a local CLI distributed as installable binaries, with Obsidian as the reader. It does not require a cloud deployment or custom web application.

## What was checked

Read the MVP, planning conversation, root README, all documents under `docs/`, all library READMEs, application/library implementations, public contracts, SQL migrations, JSON schemas, build configuration, and the unit/integration/CLI test coverage.

| Check | Result and interpretation |
| --- | --- |
| `cmake --build --preset dev` | Failed because the existing cache references the missing `/private/tmp/kc-vcpkg/scripts/buildsystems/vcpkg.cmake`. This is stale environment configuration, not proof of a C++ compilation failure. |
| `cmake --build build/verify -j 4` | Passed using cached dependency locations. CMake reported unavailable ICU components; linking reported duplicate static libraries. Neither stopped this build. |
| `ctest --test-dir build/verify --output-on-failure` | **57/57 passed.** |
| Build and CTest in `build/verify-sanitized` | **57/57 passed**, with ASan/UBSan and warnings-as-errors enabled in that build's cache. |
| Temporary install using `cmake --install` | Installation works mechanically. A probe migration added only to the installed package was found when calling the executable by absolute path, but missed when invoking `kc` through `PATH` from `/tmp`. |
| Temporary-project CLI probes | Confirmed the hard-coded tag, case-only apply rejection, same-basename source merging, help returning exit 2, non-JSON argument errors, and acceptance of invalid numeric configuration. Writer probes used deliberately seeded proposal/citation rows; they did not exercise a real model. |
| Real extraction/model environment | `pdftoppm` and `ollama` are discoverable on this PATH; `pdftotext` and `tesseract` are not. No real OCR or local-model quality result was established in this audit. |
| Release infrastructure | No tracked CI workflow, release packaging pipeline, license, changelog, or contributor/security guide was found. CMake has install rules, but no complete distribution workflow. |

These results establish a healthy existing regression suite, not production readiness. The verification builds depend on previously installed dependencies; this was not a clean-machine build. Tests largely use injected model/PDF/OCR doubles. `docs/TASK_3_VERIFICATION.md` still records an older 49-test result, unmeasured handwriting accuracy, and unconfirmed interactive Obsidian page navigation.

## Ordered tasks

Follow the numbering as the recommended completion order. P0 means a correctness or safety blocker; P1 means required release preparation; P2 means a follow-on improvement. Add regression tests with each substantive fix, then consolidate the full release checks in tasks 13–15.

### 1. Restore a reproducible development baseline — P1

**Evidence:** `CMakePresets.json`, `vcpkg.json`, `README.md`; the development preset build currently fails on the stale temporary toolchain path.

- [x] Set up a persistent vcpkg checkout and document bootstrap/version requirements alongside the pinned manifest baseline. Reconfigure from a fresh build directory instead of relying on old cache paths.
- [x] Prove the documented `dev`, `sanitized`, and `release` presets work with a clean dependency setup. Keep dependencies installed outside disposable temporary paths.
- [x] Record supported compiler/CMake/Ninja versions and selected platform targets. Investigate the ICU discovery and duplicate-library warnings; document whether ICU is intended rather than treating it as an unexplained requirement.

**Done when:** another developer can follow the README from a clean checkout to build and run the tests without inheriting this machine's caches. This is the baseline for all subsequent verification.

**Completed October 12, 2026:** [README bootstrap/build instructions](README.md#foundation-build) and [dated verification evidence](docs/DEVELOPMENT_BASELINE.md) establish the macOS arm64 development baseline. A new persistent vcpkg checkout at the unchanged manifest baseline built dependencies from source with binary-cache reuse disabled. All three presets passed in fresh configurations and an independent source snapshot: `dev` and `sanitized` each passed 57/57 tests; `release` built and passed the CLI initialization/idempotency smoke check. Final builds resolve the manifest's curl package and produce no ICU discovery or duplicate-library warnings.

### 2. Fix logical source identity before importing real note collections — P0

**Evidence:** `libs/import/src/source_importer.cpp`, `find_source` matches only display filename and source kind; compiler/extraction/review use the latest version of that logical source.

**Confirmed reproduction:** import `semester-a/Notes.md` and `semester-b/Notes.md` with different contents. Both imports return the same source ID. The second input becomes the latest version, so the first semester's notes disappear from future automatic evidence selection.

- [x] Separate “import a new logical source” from “add a revision to this source.” Give updates an explicit source ID or another stable, portable identity; basename alone must not imply a revision.
- [x] Keep byte deduplication, but define its relationship to logical identity. Global hash uniqueness currently returns the original source even for a renamed input or a historical version re-import; decide how aliases, revisions, and activation work without silently losing user intent.
- [x] Return clear import results for new source, new revision, deduplication, and partial multi-file success. `import_files` commits each file independently, but an exception currently hides earlier successful results.

**Tests / done when:** unrelated equal-basename notes remain independently selectable; intentional revisions remain idempotent; retained historical citations work; renames, identical bytes, A→B→A content, and a batch with one failed file have documented outcomes.

**Completed October 12, 2026:** Normal imports now create distinct logical
source IDs; `kc import --source SOURCE_ID FILE` explicitly appends revisions.
Global SHA-256 reuse shares immutable retained bytes without merging identities.
Current-byte revisions are idempotent; A→B→A creates a new latest revision while
preserving historical evidence and citations. Ordered per-file batch results
report committed successes and failures in human and JSON output. Migration 003
preserves version IDs, ordering, paths, pages, extraction runs, and citations.
[Source identity rules and verification](docs/SOURCE_IDENTITY.md) document
renames, aliases, shared bytes, activation, retry/upgrade behavior, and recovery
for previously merged inputs. Development and ASan/UBSan suites each passed
65/65 tests; the release build and import CLI regression passed.

### 3. Make concept identity consistent across compile, apply, search, and platforms — P0

**Evidence:** `libs/compiler/src/compiler.cpp::find_article_target` uses SQLite `COLLATE NOCASE`; `libs/vault/src/vault_writer.cpp::apply` requires exact stored-title equality; `libs/models/include/kc/models/concept.hpp` maps punctuation to slugs while SQL requires a unique slug.

**Confirmed writer reproduction:** an applied `Photosynthesis` article plus a valid update titled `photosynthesis` is approved, then rejected at apply with exit 5: `update proposal does not match its stored article`. The compiler's case-insensitive lookup selects precisely this update path.

- [ ] Resolve requests to an existing canonical identity and retain its display title and filename. Make identity checks agree across all stages.
- [ ] Define collision handling: titles such as `A-B` and `A B` share a slug, so a second concept currently reaches a database uniqueness failure during apply. Detect collisions before invoking the model or produce a deterministic disambiguated identity through a documented migration.
- [ ] Define whitespace normalization and portable filename rules, including Windows reserved names, trailing spaces/dots, and Unicode normalization/case behavior. Validate them at compile time, not only at filesystem write time.
- [ ] Fix or explicitly bound Unicode lexical matching. Compiler matching drops non-ASCII bytes, even though non-Latin titles receive supported hash slugs. Empty normalized phrases can match empty normalized page text. SQLite's current NOCASE identity does not supply general Unicode case folding.

**Tests / done when:** case-only requests update the same page; safe distinct concepts cannot collide unexpectedly; non-English automatic matching either works within stated language support or explains when curation is needed; filenames are portable across every advertised OS.

### 4. Repair generic Markdown metadata and rendering — P0

**Evidence:** `libs/vault/src/vault_writer.cpp`, especially `render_new_article`, `yaml_scalar`, and `persist_success`.

- [ ] Remove the hard-coded `probability` tag. Omit unsupported tags or add an explicit subject-neutral metadata contract. Applying a biology proposal currently creates `tags: [probability]`.
- [ ] Serialize YAML strings safely. The plain-scalar heuristic permits values such as `true`, `null`, or numeric strings that a YAML reader can interpret as non-string types. Emit empty aliases as an actual empty list rather than a null field.
- [ ] Define alias ownership on update. Updates preserve old frontmatter byte-for-byte while FTS uses the new proposal's aliases, so Obsidian metadata and CLI search can disagree. Preserve user ownership while making compiler-owned metadata consistent.
- [ ] Validate rendered headings, relationships, and reserved markers before a proposal becomes approvable. Current renderability checks happen in apply, allowing proposals that cannot be written to enter review state.
- [ ] Establish a content policy for model-generated Markdown links, raw HTML, embedded content, and multiline blocks. Check final rendered articles so injected markup cannot create misleading source references or ownership markers.

**Tests / done when:** create and update articles in biology, history, and programming; parse frontmatter with a YAML parser; check aliases, Unicode, scalar-looking titles, malicious markers, and source-link escaping. The output must be domain-neutral and metadata-consistent.

### 5. Enforce the JSON contracts without numeric coercion — P0

**Evidence:** `libs/domain/src/json.cpp` reads integers with `get<std::uint32_t>()` / `get<std::size_t>()`; runtime validation is hand-written. Schemas are used in fixture tests and model prompting, not as a universal runtime validator.

**Confirmed configuration reproduction:** repeated `kc init` accepts `schema_version: 1.9`, `timeout_seconds: -1`, and `timeout_seconds: 1.5`. Conversion can truncate fractional values or wrap negative unsigned values before semantic checks.

- [ ] Require the correct JSON primitive type and bounds before converting schema version, timeout, page number, and citation offsets. Reject booleans, floats, negatives, overflow, and numeric strings where integers are required.
- [ ] Align standalone schemas, embedded prompt schema, and C++ validation. ID patterns currently vary between prefix-only checks, arbitrary-length patterns, and exact 26-character ULIDs.
- [ ] Require meaningful article content, not merely a nonempty sections array containing empty blocks or whitespace-only prose. Define whether repeated section keys are intentional and validate accordingly.
- [ ] Consolidate shared semantic checks so the compiler, reviewer, model runner, and writer agree on validity. Keep strict rejection of additional properties.

**Tests / done when:** run a common set of valid and invalid contract fixtures against runtime parsing and schema validation, covering every numeric boundary and structural mismatch. Invalid output must never become an approved proposal or article.

### 6. Close extraction integrity, path, and concurrency gaps — P0

**Evidence:** `libs/extraction/src/extractor.cpp`, `libs/import/src/source_importer.cpp`, `libs/storage/src/project_initializer.cpp`, and `libs/compiler/src/compiler.cpp::persist_proposal`. Vault writing already has stronger canonical containment and source-hash checks than earlier stages.

- [ ] Use canonical containment checks consistently for state, source storage, cache, staging, and initialization. Lexically relative paths can still traverse symlinked directories outside the project. Reject unsupported overlapping configurations such as placing state or generated output inside immutable source storage.
- [ ] Verify retained source hashes before extraction and cache reuse; extraction currently reads the retained file without comparing its recorded digest. Reject or diagnose missing/tampered source files and images early.
- [ ] Capture selected page hashes/revisions and recheck them under the proposal-persistence transaction. A page can be corrected or force-extracted while model generation is running; current proposal persistence does not verify that its in-memory evidence is still current.
- [ ] Serialize or stage concurrent extraction attempts. Page images are overwritten before the final citation check in `persist_result`; the database lock alone does not protect the image files throughout the operation.
- [ ] Validate UTF-8 and reject embedded NULs in supported text inputs. SQLite text readers construct strings from null-terminated pointers, which can truncate embedded NUL content and make hashes/offsets disagree.

**Tests / done when:** use isolated fixtures for symlink escapes, retained-file tampering, invalid encoding, missing images, and two-process extraction/correction/compile races. A proposal must always refer to the exact reviewed evidence used to generate it.

### 7. Preserve accumulated knowledge during concept updates — P0

**Evidence:** `Compiler::compile` supplies title, operation, article ID, and selected pages; `libs/models/src/concept_prompt.cpp` receives no current approved content. `replace_managed_content` replaces the complete managed region, and article citations are replaced wholesale.

- [ ] Supply the prior approved structured content and its retained evidence when generating an update, or implement deterministic section/block reconciliation around new content.
- [ ] Make removals and changed claims explicit in review. A compilation restricted to one newly imported source must not silently discard older approved examples, explanations, or relationships.
- [ ] Preserve citations to earlier immutable versions when those claims are retained. Latest-only evidence selection is appropriate for new source revisions, but cannot by itself represent the article's accumulated knowledge.
- [ ] Add a base article/content revision to update proposals. Detect a changed managed region between review and apply; require renewed review while still preserving unrelated user additions.
- [ ] Define overwrite/regeneration versus additive-update semantics in the contracts and CLI. Document the ownership of manual edits inside the managed section.

**Tests / done when:** compile a concept from semester A, later add semester B notes, then update with `--source` for B. A's supported knowledge stays unless a reviewed removal is explicit; B's additions have exact citations; user-authored surrounding text remains unchanged. Test source revisions, stale proposals, duplicate additions, and manual vault changes.

### 8. Complete the promised review experience — P0

**Evidence:** `docs/CLI_CONTRACT.md` and `MVP.md` promise diffs; `ReviewService::review_proposal` and `proposal show` provide proposal content and citation evidence but no current-versus-proposed rendered diff. Extraction inspection prints an image path rather than displaying the page.

- [ ] Share a pure rendering/preview component between review and apply so users approve the exact Markdown that will be written, including metadata, footnotes, and protected content boundaries.
- [ ] Show additions/removals, destination, untracked collisions, and validation failures for create and update proposals. Include machine-readable preview/diff data.
- [ ] Make source images easy to open beside extracted text in the terminal workflow. A CLI is sufficient, but users should not need to reconstruct image paths manually.
- [ ] Add an explicit “reviewed, text unchanged” action for OCR. Currently users must submit replacement text just to change its review status.
- [ ] Provide a provenance-preserving correction path after a proposal has cited a page. Even rejected/superseded proposals currently block correction and force extraction forever. Introduce extraction revisions/new page identities rather than mutating historical evidence.
- [ ] Allow users to revoke an unused approval or reject approved work through a defined transition, with no implicit vault write.

**Tests / done when:** inspect/correct/accept an OCR page, preview an update with a deletion and a collision, approve the exact preview, and fix an extraction error discovered after rejection. Every action remains separate from apply and retains the old citation history.

### 9. Make apply recoverable across crashes and external edits — P0

**Evidence:** `VaultWriter::apply` holds a SQLite write transaction and restores article bytes on caught post-write failures. This is useful exception recovery, but a process crash after rename and before database commit bypasses the catch block. POSIX writes flush streams without an explicit filesystem durability barrier.

- [ ] Add a durable apply journal/recovery protocol covering backup creation, source copies, article replacement, and database publication. Recover interrupted runs when opening the project.
- [ ] Define and implement durability guarantees for temporary files, backups, rename, and parent directories on supported systems.
- [ ] Detect external editor changes between preview, reading the file, and replacement. A SQLite lock serializes compiler processes; it cannot lock out Obsidian or another editor.
- [ ] Provide a supported history/restore or rollback command that restores Markdown, citations, audit state, and FTS together, refusing to overwrite newer user changes without explicit review.
- [ ] Handle orphaned temporary files/source copies/backups and interrupted `running` extraction records through safe reconciliation.

**Tests / done when:** inject failure/termination before and after each durable step, restart, and verify that article bytes, proposal status, citations, and search agree. Test full disk, permission denial, locked files, failed commit, and failed restoration. Existing success-path tests do not establish these guarantees.

### 10. Bound real-world workloads and adapter execution — P1

**Evidence:** compiler selection collects all matching full pages; Markdown/TXT extraction reads the whole source into memory; Poppler renders all pages before processing; `LocalCommandRunner` waits synchronously without a deadline. HTTP responses have a 16 MiB ceiling and a timeout, but input/context budgets are absent.

- [ ] Set configurable source-size, page-count, render-size, and model-context budgets. Rank/select bounded evidence and explain omissions before generation; preserve original offsets when segmenting text.
- [ ] Add subprocess timeouts, cancellation, process-tree cleanup, controlled stderr capture, and page-level progress. A stalled PDF/OCR process must not hang indefinitely.
- [ ] Handle blank pages separately from failed OCR so legitimate blank pages do not make the entire extraction appear broken.
- [ ] Surface distinct model failures: unavailable service, missing model, timeout, malformed response, and citation validation. Audit fixed safe error categories while giving actionable user guidance.
- [ ] Consider bounded retries for transient failures and invalid structured output only after measuring real-model behavior. Retry attempts must not create duplicate proposals or leak untrusted responses into logs.
- [ ] Test Windows subprocess argument quoting and stdout isolation. `_spawnvp` currently lacks the POSIX branch's stdout redirection, threatening the one-JSON-document contract.

**Tests / done when:** a semester-sized corpus processes within documented memory/disk/context limits; malformed and stalled inputs terminate cleanly; cancellation is recoverable; one failed page has a clear status and retry path.

### 11. Finish CLI diagnostics and output behavior — P1

**Evidence:** `kc doctor` and `kc status` are documented but not registered in `apps/kc/main.cpp`. CLI parse exceptions call `app.exit(error)` and then always return 2.

- [ ] Implement `doctor`: configuration/schema compatibility, path safety/writability, SQLite/FTS availability, tool versions, OCR languages, local model availability, and recovery issues. Local-only operation should not require internet access.
- [ ] Implement `status` plus source listing so a returning user can discover source IDs, unextracted/unreviewed/failed pages, pending proposals, and applied articles without querying SQLite.
- [ ] Return successful exit codes for `--help` and `--version`; return the parser's intended code where appropriate.
- [ ] Emit the documented error envelope for invalid arguments in JSON mode. `kc --json search` currently writes plain parser diagnostics to stderr and no JSON object to stdout.
- [ ] Clarify `--quiet`: the implementation suppresses many human-readable results, while the contract describes suppressing progress. Standardize result/progress streams and test option placement at each command level.
- [ ] Implement the promised redacted internal diagnostic log or revise the contract explicitly. A logs directory exists, but catch handlers generally print `error.what()` without writing such a log.

**Tests / done when:** every advertised command exists, every normal/help/error path has the documented exit and output shape, and missing dependency messages tell a user how to recover.

### 12. Add integrity checks and rebuildable search — P1

**Evidence:** `SearchIndex` reads committed FTS state; indexing runs only during apply. Manual Obsidian edits/deletions/renames are not reconciled. `article_citations` retains page associations, while exact spans remain in historical proposal records.

- [ ] Add explicit check/reindex operations that report missing or changed articles, metadata drift, malformed markers, missing/corrupt source copies, and invalid citation history. Rebuild indexes without calling a model or writing article content.
- [ ] Define whether unmanaged user additions and edited aliases should be searchable. The current index includes the full file at apply time and then becomes stale until another apply.
- [ ] Resolve related links to known concept identities where possible; report unresolved links rather than pretending every cited relationship points to an existing page.
- [ ] Add a small deterministic linter for these structural checks. The full semantic graph linter is deferred, but filesystem/database consistency belongs in the first public release.

**Tests / done when:** delete/edit/rename a generated file, change an alias, corrupt an FTS row, or remove a copied source. Checks report the problem and reindex/repair restores consistency without destructive inference.

### 13. Prove the MVP with real inputs and stronger end-to-end tests — P1

**Evidence:** `tests/integration/end_to_end_tests.cpp` uses a synthetic one-page PDF/image and deterministic model, targeting Markov Chains. Other tests cover biology selection, curated evidence, and non-mathematical model identity separately. CLI compile tests cover missing evidence rather than successful generation.

- [ ] Add full create → approve → apply → search journeys for at least two unrelated domains and curated evidence, with multiple pending concepts in the same project. Include an update that preserves earlier knowledge.
- [ ] Add real, redistributable PDF fixtures: text PDF, typed scan, mixed native/scanned pages, blank page, multi-page document, malformed/encrypted input, and non-ASCII text. Exercise installed Poppler/Tesseract in a clearly labeled adapter test job.
- [ ] Add a successful black-box CLI journey through a deterministic local HTTP fixture server. Test the actual curl boundary: response limits, timeout, HTTP failures, malformed envelopes, and no accidental proxy/redirect use.
- [ ] Run a separately labeled real Ollama/Gemma evaluation. Record model/runtime versions, hardware, context settings, latency, schema success rate, citation success rate, and human-assessed usefulness. Byte-offset citations need evidence that a real model can produce them reliably; deterministic doubles cannot establish that.
- [ ] Manually complete the current MVP's three-domain acceptance set, including automatic and curated selection, a scanned PDF, and several independently compiled concepts.
- [ ] Use actual Goodnotes/handwritten notes for the personal-use evaluation, with permission and outside committed fixtures where appropriate. Measure prose and mathematical notation separately; document correction time. Add a better pluggable local OCR/vision provider if the baseline cannot serve these notes.
- [ ] Click generated citations in Obsidian and verify PDF page navigation, aliases, links, and Markdown rendering. Record supported behavior and any page-navigation fallback.
- [ ] Test a representative 20–30-document collection and a longer notes bundle; record memory, disk growth, extraction/model time, retrieval precision, and review effort.

**Done when:** update `docs/TASK_3_VERIFICATION.md` with dated, reproducible results and artifacts. Keep offline tests, real-runtime tests, and human quality judgments distinct. Do not advertise handwriting quality or source entailment based solely on matching citation substrings: exact-span validity proves provenance, not that the cited quote supports the generated claim.

### 14. Put release checks into CI — P1

**Evidence:** no tracked CI configuration; platform branches exist for subprocesses and atomic replacement, but this audit ran only on macOS.

- [ ] Add a clean build/test matrix for each advertised platform, with pinned dependency setup and caching keyed to the manifest/toolchain.
- [ ] Require normal tests and an appropriate sanitizer job; build release artifacts independently of test binaries. Keep real-model evaluation optional/separate from deterministic PR checks.
- [ ] Run regression tests from tasks 2–12 and installed-package smoke tests from task 15. Add focused malformed-contract/path fuzzing where it improves these trust boundaries.
- [ ] Check formatting, compiler warnings, and schema/document examples. Prefer small service-level changes and maintain the existing readable, documented architecture.

**Done when:** a clean CI run verifies supported targets and produces reproducible release candidates. A local green suite must not be the only release gate.

### 15. Fix installation and produce deployable release artifacts — P0 installation fix; P1 packaging

**Evidence:** `apps/kc/main.cpp::migration_directory` derives the install prefix from `absolute(argv[0])`. A bare `kc` invoked through PATH is treated as if it lived in the current directory. It falls back to the compiled-in developer source path.

- [ ] Resolve the actual running executable on each supported OS or use a relocation-safe resource layout. Locate packaged migrations when invoked via PATH, absolute path, symlink, or another working directory. Fail clearly if resources are missing; release builds must not depend on a developer checkout.
- [ ] Refuse unsupported/newer database schemas and detect missing or altered migration history. Define upgrade behavior and create a consistent project backup before migrations.
- [ ] Produce versioned platform archives/installers with the executable, migrations, required dynamic libraries, license/third-party notices, and checksums. Test dependency resolution using the release artifact, not the build tree.
- [ ] Decide which external tools are bundled versus separately installed. Document Poppler, Tesseract/language data, and Ollama/model setup and hardware requirements. State that model weights are downloaded separately if that is the release policy.
- [ ] Test install, relocation, upgrade from schema 1/current schema, and uninstall on a machine without the source checkout, vcpkg cache, or developer tools. Keep user projects intact on uninstall.
- [ ] Set version strings from one source of truth and define release tagging/changelog generation. Apply signing/notarization where required by the selected distribution channel.

**Done when:** a new user can install a release, run `kc doctor`, initialize from any directory, and complete the supported workflow. For the current architecture, this is the deployment milestone; a hosted backend is outside MVP scope.

### 16. Finish onboarding, ownership, and release documentation — P1

- [ ] Add a license and review redistribution obligations for bundled dependencies/tools. Include third-party notices in releases.
- [ ] Replace source-build-only onboarding with installation and first-success instructions: configure local model, import, recover source IDs, extract/review, compile, preview/approve/apply, and search/open in Obsidian.
- [ ] Document project backup/restore, migrations, source revision rules, correction history, managed-region ownership, supported file encodings, local privacy boundaries, and troubleshooting.
- [ ] State that an explicitly configured remote Ollama-compatible endpoint receives evidence; the current adapter accepts arbitrary HTTP(S) hosts. Make local-only versus remote processing an explicit configuration policy before claiming guaranteed on-device processing.
- [ ] Reconcile stale docs: doctor/status, the absent diff, generic metadata, strict schemas, test counts, and unsupported API-key configuration. Archive `PLANNING.md` as historical context instead of treating every old proposal as a release promise.
- [ ] Add concise contribution, issue-reporting, security-reporting, and changelog guidance. Keep private notes, runtime state, model weights, and secrets out of repository examples and support logs.

**Done when:** a fresh user can finish the tutorial and recover from common failures using only release documentation; product claims match measured behavior.

### 17. Pilot the release and publish portfolio evidence — P1 release gate; P2 polish

- [ ] Run a tagged release candidate on your own notes and with a small set of consenting users. Track failed imports, missing concepts, incorrect citations, destructive updates, setup friction, and review time; resolve release blockers before publishing v0.1.
- [ ] Publish a safe demo corpus and sample multi-domain vault, with a short recording showing import → review → compile → diff → approve → apply → search → source page. Include an update months later that retains earlier learning.
- [ ] Add an architecture walkthrough explaining the model capability boundary, immutable provenance, transaction/file recovery design, and tradeoffs in evidence selection and local OCR.
- [ ] Publish honest benchmark/evaluation results with hardware, corpus size, runtime versions, and limitations. Show regression coverage and actual release/CI evidence rather than just a test-count badge.
- [ ] Describe your contribution through concrete engineering outcomes: source identity/versioning, exact-span validation, safe update ownership, crash recovery, and reproducible cross-platform distribution. Use measured adoption/performance only when it exists.

**Done when:** the public repository has an installable tagged release, a reproducible useful demo, documented engineering decisions, and evidence that the core promise works on real notes.

## Release acceptance checklist

A public local-first v0.1 is ready when all of the following are true:

- [ ] Distinct sources/concepts retain stable identity; no filename, slug, capitalization, or Unicode surprise loses knowledge.
- [ ] Every proposal is strictly validated against immutable reviewed evidence, and update removals are explicit.
- [ ] Review displays the actual rendered change; approval never writes the vault.
- [ ] Apply preserves user content and recovers consistently from failed writes, crashes, and stale approvals.
- [ ] Search/metadata/citations remain consistent and can be checked or rebuilt.
- [ ] The three-domain manual MVP exercise, scanned-PDF workflow, real local-model evaluation, and Obsidian click-through are recorded.
- [ ] Clean supported-platform CI passes; installation works without the source tree or old caches.
- [ ] Setup, privacy, supported formats/languages, limitations, recovery, license, and dependency requirements are documented.
- [ ] A pilot finds no unresolved data-loss or provenance failure.

## Keep out of the first release unless the scope changes

These are valuable extensions from the planning conversation, not prerequisites for the current MVP:

1. Optional API-key model/OCR providers. Implement later through the existing interfaces with explicit evidence-sharing consent and secrets from environment/keychain.
2. A loopback review/search UI (`libs/server` is only a placeholder). Add it after review/rendering services are reliable; enforce loopback binding, origin/auth checks, and safe asset access if implemented.
3. Topic maps, bulk concept discovery, embeddings, alias-based reconciliation, semantic search, and conflict suggestions.
4. Watch mode, cloud sync, a custom editor, mobile clients, and a graph visualizer.

The main next milestone is a trustworthy, installable CLI that can accumulate several useful concepts from your real notes. Build outward from that verified loop.
