Make one narrow end-to-end prototype: **compile your probability notes into a searchable Markov Chains article.**

Call it a “Personal Probability Compiler.” Its only job is:

1. Import a few of your real Goodnotes PDFs and Markdown notes.
2. Extract their text locally, retaining page numbers and images.
3. Find notes related to Markov chains.
4. Use a local model to draft one `Markov Chains.md` article from those notes.
5. Link every section or claim back to the original note page.
6. Open the article in Obsidian.

Success means you can search “Markov chains,” read a useful article in language close to your own notes, and jump back to the pages where you learned it.

A good first article structure is:

```
# Markov Chains

## My working explanation
...

## Key ideas
- State
- Transition probability
- Markov property
- Stationary distribution

## Example
...

## Related concepts
- [[Conditional Probability]]
- [[Random Variables]]
- [[Random Walks]]

## Sources
- Probability Notes, Week 8, page 3
- Probability Notes, Week 9, page 1
```

Build the prototype in this order:

1. **Local import and extraction** — support Markdown plus a small set of your actual PDFs.
2. **Extraction review** — show the text beside each source page, because handwriting/OCR quality determines everything downstream.
3. **One local-model prompt** — turn selected extracted pages into schema-validated content for one article.
4. **Markdown writer** — create the page in an Obsidian vault, preserving source links.
5. **Simple search** — search article titles and article text; semantic search can come later.
6. **Expand to five concepts** — for example Markov Chains, Conditional Probability, Bayes’ Rule, Random Variables, and Random Walks.