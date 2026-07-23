THIS IS A CONVERSATION I HAD TO PLAN THIS PROJECT





The Obsidian LLM Wiki is a phenomenal project because it directly implements th…
here is a writeup for a project that I want to do. I want to make a product that I can post for other people to use as well that compiles all the knowledge that you accumulate over time from notes and learning.
Explain how I can execute this plan. Do not change any files yet

Tuesday 11:43 PM

Worked for 55s
Your write-up has a strong core idea: turn scattered source material into a maintained, inspectable knowledge base rather than repeatedly searching raw document chunks.

For a product other people can trust, I’d build it as a local-first knowledge compiler, with the Obsidian vault as one output—not the entire source of truth. The key is provenance: every generated claim should link back to the source and location that supports it.

User imports notes / PDFs
Extract + normalize text
Build proposed knowledgechanges
Validate schema, links,citations
User reviews / approves
Write Markdown vault +update local state
Graph lint + conflict report
Mermaid source codeflowchart LR
  A["User imports notes / PDFs"] --> B["Extract + normalize text"]
  B --> C["Build proposed knowledge changes"]
  C --> D["Validate schema, links, citations"]
  D --> E["User reviews / approves"]
  E --> F["Write Markdown vault + update local state"]
  F --> G["Graph lint + conflict report"]
Start with a narrow MVP
Do not begin with a daemon, embeddings, automatic merging, or a 200-document stress test. Those are worthwhile later, but an MVP should prove one valuable loop:

A user imports a .md, .txt, or text-based PDF.
The system extracts a small set of atomic claims, concepts, definitions, and source citations.
It proposes Markdown pages and edits as a preview.
The user accepts or rejects the changes.
It produces a clean Obsidian-compatible vault with working [[links]].
Initial page format:

---
title: Backpropagation
tags: [machine-learning]
sources:
  - source_id: paper-2026-001
---

# Backpropagation

## Summary
...

## Claims
- ...

## Related concepts
- [[gradient-descent]]

## Sources
- [paper-2026-001, p. 4](...)
That alone is a usable product. It also lets you test whether people actually find the compiled output better than their existing notes.

Make a few important changes to the original architecture
The write-up is right about compilation, but I would adjust these assumptions:

A compiled wiki complements RAG; it does not replace it. The wiki is excellent for durable concepts and synthesis. Raw-source search remains essential for answering “where exactly did this claim come from?”
Never let an LLM freely overwrite a user’s vault. Generate a proposed operation plan, validate it, show a diff, then apply it. Preserve user-authored sections.
Use a local database for state. Markdown is the human-facing artifact; SQLite should track imports, content hashes, concepts, source references, model/version information, operations, and failures.
Treat “contradictions” as review candidates, not facts. Models can flag potentially conflicting claims, but they should not decide that a newer paper invalidates an older one.
Avoid a permanent filesystem watcher early on. A deterministic compile command and a manual “Import folder” action are easier to make reliable. Add watch mode once processing, retries, and duplicate detection are solid.
Recommended product shape
Start as a cross-platform CLI plus a local web/desktop interface:

kc init — creates a project and configuration.
kc import <file> — copies or registers a source and hashes it.
kc compile — creates proposed knowledge updates.
kc review — displays diffs, citations, and validation results.
kc apply — writes approved changes to the vault.
kc lint — checks links, metadata, duplicate concepts, and stale citations.
wiki doctor — diagnoses configuration or provider problems.
Later, wrap this core in a desktop UI. That gives nontechnical users an approachable product without tying your core logic to a GUI.

Core data model
Keep three durable layers:

Layer	Purpose	Ownership
sources/	Original imported files or immutable references	User
.knowledge-compiler/	SQLite state, extraction cache, logs, proposed operations	Application
vault/	Obsidian-compatible generated and user-edited Markdown	Shared, carefully managed
Use a stable source ID and content hash for every import. Each generated statement should retain:

source ID
page/section/character range where possible
extraction timestamp
model and prompt/version
confidence or review status
This is what turns “AI-generated notes” into a serious knowledge system.

Compilation pipeline
Implement the compiler as a sequence of idempotent stages:

Discover and deduplicate
Hash files; skip already-processed sources unless they changed.

Extract text
Begin with Markdown, TXT, and text PDFs. Add OCR and handwriting only after the fundamentals work—handwriting quality will otherwise dominate your support burden.

Normalize and segment
Preserve headings, page numbers, and source offsets.

Create structured source facts
Require schema-constrained model output: claims, definitions, entities, candidate concepts, links, and evidence spans.

Reconcile against existing concepts
Retrieve only relevant concept summaries and source facts, then ask whether each item is new, additive, duplicate, or conflicting.

Generate a change set
The model returns operations such as create_page, add_claim, add_source, add_link, or flag_conflict—not arbitrary Markdown edits.

Validate before writing
Validate JSON schema, allowed tags, filename rules, source references, link targets, and protected user content.

Preview, approve, apply
Write atomically and retain an audit log so every compilation is reversible.

Graph linter: make this the standout feature
Your linter is a genuinely good differentiator, but give it precise, defensible checks:

broken [[Obsidian links]]
duplicate or near-duplicate concept pages
orphaned concepts
invalid frontmatter / tags outside the schema
claims lacking citations
citations pointing to removed or changed sources
circular dependency paths, where those matter
possible semantic conflicts, clearly labeled for human review
Build a graph from page IDs rather than filenames wherever possible; filenames can change. Export graph diagnostics as both Markdown and JSON so the UI can visualize them later.

Technology recommendation
If the goal is primarily a widely usable product, I would not make C++ a requirement. C++ is a valid choice, but it makes PDF extraction, OAuth, UI, packaging, HTTP, and iteration slower.

Two good paths:

Product-first: Rust core, SQLite, local HTTP API, Tauri desktop app, Obsidian-compatible Markdown output.
C++ portfolio-first: C++23, CMake, SQLite, libcurl/an HTTP client, nlohmann/json, a PDF-text extraction adapter, and a lightweight local UI later.
Either way, isolate the LLM provider behind an interface. Use structured outputs/JSON schemas, retries with exponential backoff, request logging with secrets redacted, and per-project API settings.

A practical first 8 weeks
Week 1: Define the product contract: supported source types, exact Markdown schema, concept-page structure, and what users must approve.
Week 2: Build project initialization, source import, hashing, and SQLite state.
Week 3: Support TXT and Markdown extraction; save normalized source records.
Week 4: Add one LLM-backed extraction call that produces schema-validated claims with citations.
Week 5: Generate proposed concept pages and show a human-readable diff.
Week 6: Apply approved changes atomically; add rollback/audit history.
Week 7: Implement the deterministic link and metadata linter.
Week 8: Test with 20–30 real documents; interview a few Obsidian users and use their failures to set the next scope.
What success looks like
Your first release succeeds if a user can import 20 documents, approve a compilation run in a few minutes, navigate the resulting vault confidently, trace important claims to sources, and re-run compilation without duplicate or destructive edits.

The project becomes compelling when it earns trust—not merely when it generates a large graph. The review workflow, stable citations, reversible changes, and clean Markdown are the features that make it something people can actually adopt.

No files were changed.

Tuesday 11:44 PM
explain the outward facing user product in clear terms. I want to see if it is aligned with my vision

2:45 PM
My interpretation of your vision is:

A personal knowledge compiler that turns everything a person reads, writes, or learns into a continuously improving, cross-linked knowledge base they can own and inspect.

It is not primarily a chatbot, note-taking app, or document search tool. The outward-facing product is a system that helps users build and maintain a durable “second brain.”

What the user experiences
A user installs the app and connects it to an Obsidian vault or another Markdown folder.

They then add sources such as:

research papers
books and articles
lecture notes
handwritten-note exports
personal observations
existing Markdown files
The product reads these sources and proposes updates to the user’s knowledge base.

For example, after importing a paper about neural networks, it might propose:

creating a new Backpropagation concept page
adding a definition to Gradient Descent
linking both pages to Optimization
adding a source-backed claim to an existing page
flagging a possible disagreement with an older source
The user reviews those proposed changes, accepts or rejects them, and the product updates the vault.

The central product loop
The product should feel like this:

Add knowledge
Drop in a file, paste text, or import notes.

Compile
The system extracts concepts, claims, definitions, relationships, and citations.

Review
The user sees proposed pages and edits before anything changes.

Apply
Approved changes are written into the knowledge base.

Explore
The user navigates a growing graph of connected concepts.

Maintain
The system identifies broken links, duplicate concepts, uncited claims, outdated material, and possible contradictions.

The important distinction is that the product does not just answer a question once. It compounds the user’s knowledge over months and years.

What the user ultimately receives
The main output is a clean, navigable knowledge graph consisting of:

concept pages
source pages
indexes and topic maps
backlinks and related concepts
citations to original material
review queues for conflicts or uncertainty
a history of how the knowledge base changed
The user should be able to open the result in Obsidian and still own the files independently of your application.

Example user story
A user studying machine learning imports ten papers over several weeks.

The product gradually builds pages such as:

Gradient Descent
Backpropagation
Activation Functions
Optimization
Generalization
When a new paper introduces a different optimization method, the product does not silently rewrite the existing wiki. It shows:

“This source adds a new method related to Optimization and may conflict with a claim from 2022. Review suggested changes.”

The user approves the useful additions, rejects poor ones, and ends up with a more coherent knowledge base than they could have maintained manually.

The product’s promise
The promise to users is:

“Give us the material you are learning, and we will help turn it into an organized, connected, source-grounded knowledge system that remains yours.”

The strongest differentiators are:

knowledge compounds instead of being repeatedly re-summarized
generated claims remain traceable to sources
users approve changes before they affect their files
the system detects structural problems in the knowledge graph
the output is portable Markdown, not a locked-in database
What it should not be
To keep the vision clear, the product should not initially position itself as:

an autonomous replacement for thinking
a generic chat-with-your-PDF app
a fully automatic truth engine
a social note-sharing platform
a system that silently edits a user’s vault
a replacement for raw-source search
The product’s role is closer to an editor, librarian, and compiler: it organizes and connects knowledge while leaving judgment with the user.

The key product decision
There are two possible interpretations of your idea:

A developer-oriented compiler: primarily a C++ systems project and command-line tool.
A general user product: a friendly application that hides the compiler machinery and helps anyone build a personal knowledge graph.
Your write-up describes the first, but your goal of sharing it with other people points toward the second. The C++ compiler can remain the technical core, while the outward-facing product should be presented as:

“An app that continuously compiles your learning into a personal, source-grounded wiki.”

That appears aligned with the vision in your write-up.

2:45 PM
My ideal user is myself. I want to be able to upload papers and such, but mainly I want to upload notes that I have taken while watching or reading something.  I have notes from a semester of probability that I might want to look back on. The main purpose is to provide the user a way to easily search for and read articles about concepts that they have already learned. If I need to look back on how Markov chains work, I want to be able to search for and read an article about markov chains that were based off of my own notes. This way, the articles are already close to my own words but I can also see the connections to other concepts that I have also already learned. Does this change the plan at all? I would be mainly uploading pdf scans of notes, markdown files, or pdfs of goodnotes files. I want the topics to be mainly searched by topic. Even though I am uploading a semester worth of notes, it should be accumulating knowledge on a topic basis, because i might have one off learned about the topic before or after said semester. When I search algorithms, I don't want a recreation of the textbook i read in my algorithms course but in my own words, I want to see all of the algorithms that I have learned over time

2:58 PM
Yes—this sharpens and simplifies the product.

The product is not mainly “turn documents into a wiki.” It is:

A personal reference library that turns your learning notes, across time and sources, into readable articles organized by concept.

Your notes are the primary material; papers and textbooks are supporting evidence. The central question is not “What did this PDF say?” but:

“What have I learned about Markov chains, in my own language, and what else does it connect to?”

What the product should feel like
You upload:

a scanned PDF of handwritten probability notes
Goodnotes PDF exports
Markdown notes
occasionally, a paper, textbook excerpt, or article
The app reads and indexes them. Over time, it builds a durable page for each concept you have encountered.

When you search Markov chains, you find a coherent personal article—not a chat response and not a replay of a specific lecture:

an explanation synthesized primarily from your notes
definitions and examples you wrote down
links to [[conditional probability]], [[stationary distributions]], and [[random walks]]
source references showing which notes the article came from
perhaps a small “learned over time” section showing that you first saw it in probability, then later in an ML context
When you search algorithms, you should not receive one enormous regenerated algorithms textbook. You should see a topic map:

Algorithms
├── Sorting
│   ├── Merge sort
│   ├── Quick sort
│   └── Heap sort
├── Graph algorithms
│   ├── BFS
│   ├── DFS
│   └── Dijkstra’s algorithm
├── Dynamic programming
└── Greedy algorithms
Each item is its own accumulated article. The Algorithms page is an overview and navigation hub, while your knowledge about each individual algorithm continues to grow whenever you encounter it again.

The product model should change to two levels
Layer	What it is
Concept article	One durable page for a specific idea, such as Markov chains, Dijkstra’s algorithm, or Bayes’ rule.
Topic map	A navigable overview, such as Probability, Algorithms, Linear Algebra, or Machine Learning.
A note from a probability class may update several concept articles and topic maps. The original course structure matters as provenance, but it should not determine the final organization.

For example, the system may identify that a page of notes contributes to:

Conditional Probability
Bayes’ Rule
Markov Chains
Probability
Later, notes from a machine-learning course might add to Markov Chains again—even though the courses occurred months apart.

That is the compounding behavior you want.

Revised core user flow
You upload a semester’s Goodnotes PDFs or Markdown notes.
The system extracts text—including OCR for scans and handwriting.
It identifies concepts in each source.
It finds or creates the relevant concept articles.
It proposes additions, with links back to the source notes.
You approve the changes.
Months later, new notes enrich the same articles rather than creating a separate course-shaped knowledge silo.
The main interface should begin with search and browsing, not an import dashboard:

Search: [ markov chains                       ]

Markov Chains
Personal reference · 8 note sources · Probability

Related:
Conditional Probability · Random Walks · Hidden Markov Models

Sources:
Probability notes, Week 7
ML reading notes, March 2026
What changes technically
The original plan’s compiler and graph-linter ideas still fit, but the priorities change:

Handwriting and PDF extraction become essential.
Goodnotes exports and scanned pages will be the hardest and most important input type. Build the MVP around OCR quality and retaining page-level source references.

Concept reconciliation becomes the core capability.
The system must decide that a note from last semester and a note from today both belong to the same Markov Chains article. This is more valuable than a high-performance file watcher.

Articles must be incremental.
Each concept page should be updated in sections, not repeatedly regenerated from scratch. Preserve prior approved content and append or reconcile new material.

The system should preserve your phrasing.
Make “ground explanations in the user’s notes” a product rule. It can improve organization and clarity, but should show source snippets and avoid inventing a generic textbook voice.

Search is primarily concept-first.
Start with title, aliases, topic, and full-text search. Add semantic search later for queries like “that idea where probabilities depend only on the previous state.”

Topic maps matter more than a generic graph view.
A graph is useful for exploration, but a clean Probability or Algorithms map will likely be far more useful day to day.

The right MVP
The first version only needs to do this well:

Upload notes → extract concepts → create/update source-linked concept articles → search and read them.

A strong first demo would be importing your probability notes and producing a browsable collection of articles for Random Variables, Conditional Probability, Bayes’ Rule, Markov Chains, and related concepts—each grounded in your own notes and linked together.

That is much more aligned with your vision than a general-purpose RAG tool or an automated textbook generator.

2:59 PM
I want to have all of the LLM and pdf processing run on a local LLM like gemma but also be available to use an API key. I personally want to use a local LLM for my setup

3:26 PM
Yes—make the product local-first by design, with API use as an optional provider choice.

Your personal setup should work with no source content leaving your machine:

Notes / scanned PDFs
        ↓
Local OCR / handwriting recognition
        ↓
Local embedding + concept matching
        ↓
Local LLM (for example, Gemma)
        ↓
Local Markdown vault + local database
An API key simply enables an alternate or hybrid path when a user wants higher-quality extraction, better handwriting recognition, or faster processing.

The important distinction
A local text LLM such as Gemma can organize and synthesize extracted text, but it is not necessarily enough to read handwritten scanned pages itself.

For PDFs, use separate local stages:

Text PDF / Markdown: extract text directly.
Scanned typed PDF: local OCR.
Handwritten Goodnotes export: render each page, then use local handwriting OCR or a local vision-language model.
LLM compilation: give the extracted, page-referenced text to the local model to identify concepts and propose article updates.
Every extracted passage should retain its page reference so that a claim in Markov Chains.md can point back to the exact note page.

What the user sees
During setup, the user chooses a processing mode:

Mode	What happens
Local only	OCR, embeddings, and article generation stay on-device. No API key required.
Hybrid	Local processing by default; the user chooses specific imports or difficult pages to send to an API.
API only	Uses the user’s chosen provider API for users who prioritize convenience or quality over local processing.
For you, the default would be Local only. The UI should clearly state something like:

“Your notes are processed on this device. No source content is sent to an external service.”

That privacy property is a meaningful part of the product, particularly for students’ notes, work material, and personal writing.

Revised product architecture
Design the compiler around replaceable providers:

                 ┌──────────────────┐
Imported note ──►│ Extraction layer │──► page-referenced plain text
                 └──────────────────┘
                           │
                 ┌─────────▼──────────┐
                 │ Local or API model │──► structured concept updates
                 └─────────┬──────────┘
                           │
                 ┌─────────▼──────────┐
                 │ Knowledge compiler │──► vault + local search index
                 └────────────────────┘
The compiler should not care whether the model is Gemma running locally or an API. It sends a structured request—“extract concepts and proposed updates in this JSON format”—and receives the same structured result.

Create interfaces for:

text extraction / OCR
vision or handwriting extraction
embeddings for concept matching
language-model generation
local search
That lets users choose components without changing the knowledge-base format or workflow.

How local compilation would work for your probability notes
Import the Goodnotes PDFs.
The app processes pages locally and records the extracted text with source-page links.
A local embedding model finds that several pages refer to Markov chains, conditional probability, and random walks.
Gemma receives the relevant extracted passages plus the existing Markov Chains article.
It proposes a source-grounded update in your preferred style.
You approve it.
The app writes the updated article and indexes it for search.
When you later upload a machine-learning note mentioning hidden Markov models, the system finds the existing Markov-chain concept locally and proposes a connection rather than creating an isolated course note.

What changes in the build plan
Local support should be in the MVP, not a later optimization:

Build imports and Markdown/TXT support.
Add local text-PDF extraction.
Add a local-model adapter and run a small model end to end.
Build the source-linked concept article workflow.
Add local semantic and keyword search.
Add scanned and handwritten-PDF processing.
Add optional API adapters only after the local path is solid.
The realistic early constraint is that handwritten-note accuracy will vary. The product should therefore show extracted text alongside the original page and let the user correct bad passages before they become permanent knowledge. That review step protects the quality of every article built from those notes.

So the revised promise becomes:

“Compile your notes into a personal, searchable knowledge library—privately on your own machine, with optional API assistance when you choose.”

3:27 PM
Should obsidian be the main way to interact with the articles created or should there also be another interface?

3:40 PM
Use both, with distinct roles.

Obsidian should be the main place to read, browse, and manually edit articles at first. It already gives you Markdown editing, backlinks, graph views, mobile access, plugins, and a familiar personal-knowledge workflow.

But the product should also have its own lightweight interface for the parts Obsidian is not designed around:

importing PDFs and note bundles
showing OCR quality and original source pages
compiling notes into proposed article updates
reviewing diffs before changes are applied
searching across concepts and raw source passages
surfacing duplicate concepts, broken links, and potential conflicts
choosing local-model versus API processing
So the clean product split is:

Surface	Primary job
Obsidian vault	Read articles, explore links, edit notes, own the files.
Knowledge Compiler app	Import, process, search, review, maintain, and explain provenance.
For your use case, the flow would be:

Open the Knowledge Compiler app.
Upload a Goodnotes PDF.
Review extracted text if needed.
Approve suggested changes to Markov Chains, Conditional Probability, etc.
Open those pages in Obsidian to read, browse, and edit them naturally.
Later, the app can include an excellent built-in article reader so someone does not need Obsidian. But I would not build a full custom note editor early; that risks recreating Obsidian before proving the compiler itself.

The architectural rule should be:

The vault is portable and user-owned. The app is an intelligent companion, not the only doorway to the user’s knowledge.

That also supports your eventual broader product: Obsidian users can adopt it immediately, while non-Obsidian users can use the built-in interface when it becomes mature.

3:40 PM
what should I make first?

3:41 PM
Make one narrow end-to-end prototype: compile your probability notes into a searchable Markov Chains article.

Call it a “Personal Probability Compiler.” Its only job is:

Import a few of your real Goodnotes PDFs and Markdown notes.
Extract their text locally, retaining page numbers and images.
Find notes related to Markov chains.
Use a local model to draft one Markov Chains.md article from those notes.
Link every section or claim back to the original note page.
Open the article in Obsidian.
Success means you can search “Markov chains,” read a useful article in language close to your own notes, and jump back to the pages where you learned it.

A good first article structure is:

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
Build the prototype in this order:

Local import and extraction — support Markdown plus a small set of your actual PDFs.
Extraction review — show the text beside each source page, because handwriting/OCR quality determines everything downstream.
One local-model prompt — turn selected extracted pages into schema-validated content for one article.
Markdown writer — create the page in an Obsidian vault, preserving source links.
Simple search — search article titles and article text; semantic search can come later.
Expand to five concepts — for example Markov Chains, Conditional Probability, Bayes’ Rule, Random Variables, and Random Walks.
Defer the daemon, full graph linter, APIs, embeddings, custom UI, and large-scale ingestion until this works on your own notes. If the resulting Markov Chains page genuinely helps you refresh the topic, you will have validated the core product in the smallest possible form.

3:41 PM

cc-wikiwiki


Environment

Changes

Local

Commit or push

Sources

The Obsidian LLM Wiki is a phenomenal project because it directly implements th…

View all

