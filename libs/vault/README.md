# Vault library

The vault library implements MVP step 2B and is the only project layer that
can write an article. `VaultWriter` accepts immutable structured proposals and
enforces the review boundary: `approve` changes proposal state only, while
`apply` accepts exactly the `approved` state.

An application:

- resolves every cited page to its immutable retained source and copies that
  source under the configured vault `_sources` directory;
- renders proposal blocks, `[[Related Concepts]]`, and page-linked footnotes;
- creates a new article or replaces only the configured `kc:managed` region;
- refuses malformed/missing markers and untracked file collisions unless the
  caller supplies the explicit overwrite option;
- backs up all previous bytes before an atomic temporary-file rename; and
- commits article identity, citations, apply audit, proposal state, and FTS
  content together only after the filesystem write succeeds.

Atomic article replacement uses same-directory rename semantics on POSIX and
Windows `MoveFileExW` with replacement enabled, so updating an existing article
does not depend on the destination being absent.

If the database commit fails after the rename, the writer restores the backup
(or removes a newly-created article), so an unrecorded application is not left
in the user vault.
