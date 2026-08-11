# Review library

The review library implements MVP step 2C as a terminal-first review surface.
It deliberately does not depend on the vault library and cannot edit article
files.

Extraction review loads the latest immutable source version and returns each
page's project-relative image path, extracted text, and extraction status as a
single record. A reviewer may replace one page's text; the replacement is
stored transactionally with a new SHA-256 and `reviewed` status. Corrections are
refused after any proposal cites the page because changing the text would
invalidate immutable citation byte offsets.

Proposal review strictly parses the immutable payload and joins every
normalized citation to its source name, page number, image path, full extracted
text, and extraction status. Listing and showing proposals are read-only.
Rejection changes only review metadata (`status`, `reviewed_at`, and optional
`review_reason`), just as approval changes review state without applying it.

The CLI exposes this layer through:

```bash
kc review extraction SOURCE_ID
kc review extraction SOURCE_ID --page 3 --text-file corrected-page.txt
kc proposal list --status pending
kc proposal show PROPOSAL_ID
kc proposal reject PROPOSAL_ID --reason "Needs a clearer example"
```
