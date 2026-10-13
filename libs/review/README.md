# Review library

The terminal review service inspects current extraction pages, verifies their
images/text, accepts OCR text unchanged, and records corrections. Referenced
pages are immutable; corrections create current successors with retained
predecessor identities. Default-viewer opening uses verified absolute argv
with an injectable runner.

Proposal review joins exact citation evidence and the read-only vault preview
capability. It cannot write articles. The pure renderer is shared with apply;
show/preview are read-only, while approval/revoke/reject change only review state.
Rejected/superseded history does not block future extraction correction.

```bash
kc review extraction SOURCE_ID --page 3 --open-image
kc review extraction SOURCE_ID --page 3 --accept
kc review extraction SOURCE_ID --page 3 --text-file corrected-page.txt
kc proposal show PROPOSAL_ID
kc proposal approve PROPOSAL_ID --preview PREVIEW_SHA256
kc proposal revoke PROPOSAL_ID
kc proposal reject PROPOSAL_ID --reason "Found an extraction error"
```

See [exact preview, JSON, correction history, and verification](../../docs/REVIEW_EXPERIENCE.md).
