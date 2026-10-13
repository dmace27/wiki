# Accumulated knowledge during concept updates

Task 7 verification: October 12, 2026. Native target: macOS arm64.

## Update contract

Normal `kc compile --concept TITLE` updates are **additive**. `--source` controls
only the new evidence sent to the model. A semester B-only update retains all
previously approved explanations, examples, relationships, and their exact
historical citations from semester A. New model output is validated against B
before deterministic reconciliation. The model cannot remove an approved claim
or introduce a citation to an old page it was never shown.

The base is the last successfully applied structured proposal, not a pending
proposal or a fresh inference from Markdown. Prior sections and blocks retain
their order, headings, text, and citations. Novel blocks append to the matching
section key/heading or to a new section; novel relationships append in order.
Identical visible block text (same section key and block kind, with whitespace
folded as in rendering) merges into the retained block, preserving its heading.
Identical relationship titles under the concept identity rules and identical
visible reasons merge their citations. Citations deduplicate by page/start
position; when that slot already exists, the approved exact span wins. Repeating
an identical contribution neither duplicates claims nor grows citation rows.

This is literal reconciliation, not semantic equivalence or contradiction
resolution. A differently worded claim is an addition, even if it conflicts
with older prose. Review displays both. The compiler does not silently decide
which semester's claim is true.

## Explicit regeneration and review

To propose replacement, deliberately request regeneration:

```bash
kc compile --concept "Markov Chains" --source src_01J... --regenerate
kc proposal show prp_01J...
kc proposal approve prp_01J... --preview PREVIEW_SHA256
kc apply prp_01J...
```

`--regenerate` requires an existing applied article. It proposes complete
replacement content from the selected sources and may remove older claims and
citations. It still creates a pending proposal; neither compile nor approval
writes the article. The mode is stored independently of the untrusted model
payload in `proposal_update_bases`.

`proposal show` displays the update mode, base apply-run ID, retained claim
count, every removed claim, and every added/replacement claim, followed by the
complete reconciled proposal and its citation evidence. Removal entries include
the original section key, heading, block kind, text, and exact citations;
relationship removals include title, reason, and citations. Changed claims in
regeneration appear as a removal plus an addition. Heading changes and lost
citation support also appear explicitly. Additive updates must have an empty
removal list; review, approval, and apply reject an additive payload that drops
any approved claim or citation.

JSON `proposal show` adds `result.update_changes`, with these fields:

| Field | Meaning |
| --- | --- |
| `mode` | `additive`, `regenerate`, or historical `legacy_unverified`. |
| `base_apply_run_id` | Durable article revision used for compilation. |
| `base_managed_sha256` | Digest of the bytes between the ownership markers. |
| `retained_claim_count` | Number of old claims retained with their citations. |
| `removed_claims` | Full old block/relationship records being removed. |
| `added_claims` | Full added/replacement records, including added support. |

Creates return `update_changes: null`. Historical updates without a recorded
base remain inspectable as `legacy_unverified`, with null base/change fields;
null means unknown, not zero removals. Pending or approved legacy updates must
be recompiled and reviewed before they can be approved/applied. The model-facing
article-proposal JSON schema remains version 1: trusted revision/mode data is
compiler-owned state, not something the model may choose.

## Evidence and source revisions

New evidence selection remains active-source/latest-version-only. Retained
claims reload their old page IDs, immutable source versions, exact byte offsets,
and normalized quotes even after a newer source revision or source archiving.
The compiler verifies retained source/image/text hashes and validates the
complete reconciled proposal against new plus historical evidence. It persists
normalized citations and frozen evidence for both in the same transaction.
Publication rechecks all captured evidence; only new selections must still be
the latest active version. Historical evidence must still be the exact retained
version. Missing or corrupt retained evidence aborts the additive update.

Every successful apply renders the complete reconciled structured proposal,
retains/copies old immutable source links, and publishes article citations, apply
history, and FTS from that same result. Citation rows are replaced with the union
represented in the approved proposal. Regeneration intentionally publishes only
its reviewed replacement citations. Search includes both semesters after an
additive apply. `selected_page_count` reports the newly selected model-input
pages, excluding retained historical pages used only for reconciliation.

## Revisions and ownership of manual changes

Migration `005_accumulated_updates.sql` adds `apply_runs.managed_sha256` and
`proposal_update_bases`. Re-run `kc init` on existing projects to apply it.
An update records the last apply-run ID and exact managed-region digest before
generation. Publication checks both again under the SQLite write transaction,
before superseding any pending work. Approval and apply independently recheck
both against the current article. Another apply invalidates the base even if
article bytes return to the same value. Apply also compares the actual region
read for replacement to the captured digest before creating backups/copies.

Frontmatter, the title outside the markers, and surrounding user notes remain
user-owned under the metadata contract. Edits outside the managed markers can
occur before compilation, during review, or after approval; they do not stale
the managed base. Task 8 additionally binds complete preview bytes: edits after
review/approval require a fresh preview and approval, preserving the new user
bytes on both failed and renewed applies. See [exact preview binding](REVIEW_EXPERIENCE.md). Invalid compiler identity
metadata or invalid ownership markers still fail the existing writer checks.

**Manual edits inside the markers are not adopted as approved knowledge.** Even
a whitespace edit invalidates the base. The compiler refuses to regenerate over
it, and approval/apply refuse a previously compiled update. Keep a copy of your
edited file, move your manual material outside the markers, and restore the
last approved managed region from a known project/file backup. Then compile a
new proposal and review/approve it. For an actual claim correction, revise/import
and extract the supporting source, use explicit regeneration, and review the
listed removals. No override flag bypasses a stale base or missing history.

Legacy applied articles have no managed digest until the first new apply. Their
first update requires the full article to match the last apply-run's recorded
full-file digest; this establishes a safe managed baseline without trusting
manual managed edits. After that first verified update, surrounding edits are
supported normally. If a legacy file has any manual edits, preserve those edits
separately and restore a known full-file snapshot matching the last applied
revision before the first update. Articles lacking applied structured history
or with corrupt history require restoration of the project history; the
compiler does not fabricate prior content from a Markdown file.

The base checks cover changes between compile/review and apply. Durable crash
recovery and locking an external editor during filesystem replacement remain
task 9; this task does not claim those guarantees. Exact citation spans prove
provenance, not semantic entailment or automatic contradiction resolution.

## Verification

The deterministic `[updates]` integration cases use actual imports, extraction,
strict model validation/auditing, compilation, review, approval, apply, and FTS:

- Semester A then B-only update, separately imported notes and intentional
  revisions of the same source; original version IDs and exact spans survive.
- Archived A remains cited while B contributes new exact-span citations;
  missing/tampered retained A evidence aborts B-only accumulation.
- Approved examples and relationships survive; user notes added before compile
  and after approval remain byte-identical; both semesters are searchable.
- Duplicate claims within one response and across updates merge citations;
  repeated compilation/application produces identical Markdown.
- Explicit regeneration lists old explanations, examples, and relationships
  as removals; compile/review/approval leave the article unchanged.
- Managed edits before compile, before approval, and after approval refuse
  writes; restoring the base allows a fresh reviewed update.
- Another apply or managed edit during generation prevents publication without
  superseding pending proposals; identical bytes with a newer apply revision
  still invalidate an old approval.
- Missing bases/history, corrupted additive removals, and legacy baseline
  upgrades fail safely; historical legacy proposals remain inspectable.

Run with the documented persistent vcpkg setup:

```bash
cmake --build --preset dev -j 4
ctest --preset dev --output-on-failure -j 4
cmake --build --preset sanitized -j 4
ctest --preset sanitized --output-on-failure -j 4
cmake --build --preset release -j 4
python3 tests/cli/accumulated_updates_smoke.py --kc build/release/apps/kc/kc
```

The standalone CLI smoke uses only Python standard libraries and a temporary
loopback-only model fixture. It exercises the actual curl/CLI boundary, JSON
and human removal reports, duplicate idempotency, stale approval refusal,
regeneration, preserved user notes, and search. A sandbox must permit binding
the temporary loopback server to run that optional check.

On October 12, 2026, development and ASan/UBSan (warnings-as-errors) suites
each passed **130/130 tests**. The release build and release CLI smoke passed.
Task 7 of `finish.md` records completion. These tests are offline
and deterministic; real-model quality and interactive Obsidian evaluation
remain later finish tasks.
