# Knowledge Compiler

Knowledge Compiler turns PDFs, scans, and Markdown learning notes into a personal,
source-grounded Obsidian knowledge base. The MVP targets one trustworthy loop:
compile probability notes into a cited, reviewable `Markov Chains.md` article.

## Foundation build

The project uses C++23, CMake, and vcpkg manifest mode. Install CMake, Ninja, and
vcpkg, set `VCPKG_ROOT`, then run:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For warnings-as-errors with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized
```

Initialize a project without starting Ollama or Tesseract:

```bash
./build/dev/apps/kc/kc --project /path/to/project init --vault vault
```

The command creates `kc.json`, project-local storage directories, and applies
all pending SQLite migrations transactionally.
