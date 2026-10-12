# Reproducible development baseline

Verified October 12, 2026 for Task 1 in `finish.md`, starting from repository
revision `34b3557` plus the Task 1 build/documentation changes.

## Selected target and tools

The supported development target is native **macOS / arm64**, using vcpkg's
`arm64-osx` triplet and Apple Clang with its matching libc++. Intel macOS, Linux,
and Windows require separate verification before joining the supported target
list. The source contains platform branches; their presence alone does not
establish a working baseline on those platforms.

| Component | Measured value |
| --- | --- |
| Host | macOS 26.6.2 (`25G83`), arm64 |
| Command Line Tools package | `26.6.0.0.1781586589` |
| Compiler | Apple Clang 21.0.0 (`clang-2100.1.1.101`), `/usr/bin/c++` |
| SDK | macOS 26.5, selected through `/Library/Developer/CommandLineTools` |
| C++ language mode | `gnu++23` |
| CMake | 4.4.0; project/presets require at least 3.25 |
| Ninja | 1.13.2; documented requirement is at least 1.11 |
| Git | 2.54.0 |
| vcpkg Git revision | `d015e31e90838a4c9dfa3eed45979bc70d9357fc` |
| vcpkg executable | `2026-04-08-e0612b42ce44e55a0e630f2ee9d3c533a63d8bc1` |

The verified compiler baseline is Apple Clang 21. The CMake/Ninja minimums are
configuration requirements; this run measured the exact versions above, not
every intermediate tool version. Keep the compiler, standard library, and SDK
consistent across configurations. This procedure reproduces dependency
versions and build/test commands; it does not promise byte-identical binaries
across SDK versions or source locations.

## Dependency setup

Follow the complete bootstrap and build commands in the root [README](../README.md).
This verification used a new full Git clone at
`$HOME/.local/share/knowledge-compiler/vcpkg`, checked out at the manifest's
existing `builtin-baseline`, then bootstrapped with `-disableMetrics`. That
checkout pins the bootstrap metadata to tool release `2026-04-08` as well as
the port definitions.

`VCPKG_INSTALLED_DIR` is set by every preset to `$VCPKG_ROOT/installed-kc`.
The first dependency installation started empty, with
`VCPKG_BINARY_SOURCES=clear`, and built all 11 packages from source. Both Debug
and Release dependency libraries were produced. The later presets reused this
newly built persistent installation. No dependency package, source directory,
or CMake prefix was borrowed from the previous `build/dev/vcpkg_installed` tree.
No dependency checkout/install path is under a temporary directory.

| Manifest dependency | Resolved version / port revision | Selected features |
| --- | --- | --- |
| CLI11 | 2.6.2 | core |
| curl | 8.20.0#1 | core, non-http, ssl, openssl |
| json-schema-validator | 2.4.0 | core |
| nlohmann-json | 3.12.0#2 | core |
| sqlite3 | 3.53.1#2 | core, fts5, json1 |
| Catch2 | 3.15.0 | core |
| OpenSSL (curl dependency) | 3.6.2 | core |
| zlib (curl dependency) | 1.3.2 | core |

The remaining three packages are vcpkg build helpers: `vcpkg-cmake` at
`2024-04-23`, `vcpkg-cmake-config` at `2024-05-23`, and `vcpkg-cmake-get-vars`
at `2025-05-29`. The installation's `vcpkg/status` file records these resolutions
and their compiler ABI hashes.

## Clean verification

The old `build/dev` directory was retained as `build/dev-before-task-1` for
inspection. New `dev`, `sanitized`, and `release` configurations were created
with `cmake --fresh --preset <name>` and built with
`cmake --build --preset <name> --parallel 4`. Final configurations use the
persistent toolchain and package paths; no `CMAKE_PREFIX_PATH` override is used.

The README commands were also repeated in an independent source snapshot made
with `git archive HEAD`, overlaid with the changed `CMakeLists.txt`,
`CMakePresets.json`, and README. That snapshot contained no `.git`, ignored
build outputs, CMake cache, or previously installed dependency tree. It built
all three presets against the fresh persistent installation from this run.

| Preset | Configuration | Result |
| --- | --- | --- |
| `dev` | Debug, `BUILD_TESTING=ON` | Build passed; **57/57 tests passed** in both source locations. |
| `sanitized` | Debug, `BUILD_TESTING=ON`, ASan/UBSan, warnings-as-errors | Build passed; **57/57 tests passed** in both source locations. |
| `release` | Release, `BUILD_TESTING=OFF` | Build passed; CLI initialization/idempotency smoke check passed in both source locations. |

The sanitizer compile commands contain `-Werror`,
`-fsanitize=address,undefined`, and `-fno-omit-frame-pointer`. The sanitizer
executable links the Apple ASan runtime. Project targets are instrumented;
the shared vcpkg dependency installation uses its normal Debug/Release flags.
The deterministic suite does not need Ollama, Poppler, Tesseract, or private
notes to run.

Release intentionally contains no CTest tests or test executables. Its smoke
check reuses the existing black-box initialization test, which checks JSON
success, configuration/database creation, and a second idempotent invocation:

```bash
cmake \
  -DKC_EXE="$PWD/build/release/apps/kc/kc" \
  -DKC_TEST_ROOT="$PWD/build/release/baseline-smoke-project" \
  -P tests/cmake/kc_init_test.cmake
```

The release executable is Mach-O arm64. `otool -L` lists only macOS frameworks,
`libSystem`, and `libc++`: curl/OpenSSL/SQLite are linked from the manifest's
static libraries, without a system curl or vcpkg dylib dependency.

Local configure/build/test logs are retained under `.cache/task-1/`, with
`checkout-` prefixes for the independent snapshot. These logs and build trees
are ignored developer artifacts. The steps and results above are the portable
verification record.

## Warning investigation

**ICU:** The baseline's SQLite port config invokes an optional ICU search for
every static library installation, regardless of the `unicode` feature.
That feature is absent from the manifest and installed feature list; the
SQLite target links Threads and `m`, with no ICU targets. The project therefore
sets `CMAKE_DISABLE_FIND_PACKAGE_ICU` while importing its dependencies. FTS5
remains enabled and is exercised by the search tests. Adding ICU-backed Unicode
behavior requires an intentional feature/contract change.

**Duplicate libraries:** CMake's older archive ordering repeated `kc_import`
and `kc_storage` after their dependents, producing Apple linker warnings.
The root build now selects CMP0156 `NEW` when available, and CMP0179 `NEW` when
available, before `project()` loads vcpkg. This ordering matters because the
vcpkg target-creation functions capture the active CMake policies. The final
link lines contain each archive once on the verified toolchain. Configuration
and build logs for all three final presets contain neither the ICU discovery
message nor compiler/linker warnings. On CMake 3.25–3.28 the older, valid archive
ordering can still produce duplicate-library warnings.

**curl selection:** `find_package(CURL CONFIG REQUIRED)` now requires the pinned
manifest package. Previously, stale verification caches selected the macOS SDK
curl. The fresh caches resolve `CURL_DIR` to the persistent vcpkg installation,
and the imported target supplies OpenSSL, zlib, and system-framework link
dependencies.

Reference behavior is documented in [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode),
[CMake preset schema 6](https://cmake.org/cmake/help/v4.0/release/3.25.html),
[CMP0156](https://cmake.org/cmake/help/latest/policy/CMP0156.html), and
[CMP0179](https://cmake.org/cmake/help/latest/policy/CMP0179.html).
