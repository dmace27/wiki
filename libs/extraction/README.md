# Extraction library

The extraction library implements MVP step 1B.

## Pipeline

- Markdown and text sources are read from their immutable retained copy and
  become one logical page with `native` text status.
- PDFs are rendered at 150 DPI with Poppler's `pdftoppm`. Every rendered page
  receives a numbered `source_pages` row and a retained PNG under
  `.knowledge-compiler/pages/<source_version_id>/`.
- Poppler's `pdftotext` attempts native extraction one page at a time.
- Pages without usable native text are passed to the configured local
  `OcrProvider`. The default adapter invokes Tesseract with the language from
  `kc.json`.
- Successful OCR is deliberately marked `ocr_unreviewed`. Failed or unusable
  OCR is stored as `failed` with empty text; it is never presented as native or
  reviewed text.

The native-text usability guard rejects whitespace and tiny PDF extraction
artifacts. Text must contain at least four visible bytes and two substantive
characters before the pipeline skips OCR. Successful nonblank OCR remains
review-gated as `ocr_unreviewed`, including short labels and formula fragments.

## Runtime dependencies

`kc init` remains offline and does not require extraction tools. PDF extraction
requires `pdftoppm` and `pdftotext` on `PATH`; OCR fallback requires
`tesseract`. Missing or failing tools produce exit code 4 and a failed
`extraction_runs` record. When rendering succeeds but OCR fails, the page image
and an explicit failed page row are retained for later review or `--force`
retry.

Both PDF and OCR integrations are interfaces. Tests inject deterministic local
adapters and command runners without contacting external services.
