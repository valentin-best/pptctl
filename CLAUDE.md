# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`pptctl` is a C++20 CLI for `.pptx` (OOXML) files: inspect, lint, diff, optimize, merge, extract. Early development (v0.5.0) — `info`, `extract`, `lint`, `diff` and `optimize` are implemented (optimize's five passes live in `src/lib/optimize_<pass>.cpp`). `merge` is being built in Phase 6: it combines decks in input order, each keeping its own masters (pipeline in `src/lib/merge.cpp`, closure import in `merge_import.cpp`, presentation-level parts in `merge_concat.cpp`); identical media are stored once (`merge_media.cpp`); `--theme` restyling and master dedupe are still to come; `fill` is a stub that prints "not yet implemented".

## Project workflow & status

Work is planned in `ROADMAP.md` / `PLAN.md` and tracked in GitHub **milestones** (`Phase 0` … `Phase 8`, version-tagged) and **Project #5** (`valentin-best/pptctl`, a private repo). Phases are sequential: each is a shippable version (`Phase 1` → v0.1.0 `info`, `Phase 2` → v0.2.0 `extract`, …).

- **To see where things stand and what to do next, run `/next`** — it reports the current phase, its open issues, and a recommended next task. Prefer it over re-deriving status by hand.
- Work issue-by-issue within the current phase. Reference the issue number in commits/PRs.
- Phases 0–5 are done and tagged (v0.1.0 … v0.5.0; see `CHANGELOG.md`). Phases 6–8 are single **epic** tracking issues (label `epic`) — break an epic into granular issues when its phase becomes current.
- **Positioning (since 2026-10-28):** pptctl is static analysis and safe surgery for decks — `lint`, `diff`, `optimize`, `merge` — not a general deck editor (that's OfficeCLI's ground). Phase 6 adds package-integrity lint rules alongside `merge`; Phase 7 is integrations (GitHub Action, pre-commit, agent skill); `fill` and library extraction are deferred. See the Positioning section of `ROADMAP.md`.
- Don't publish the roadmap, make the repo public, or post launch content until something has actually shipped (a deliberate choice — see `ROADMAP.md`).

## Build, test, run

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler with `std::format` (Apple Clang ≥ 16, LLVM Clang ≥ 17, GCC ≥ 13, MSVC ≥ 19.30). Dependencies are fetched automatically via CMake `FetchContent` — no system packages, vcpkg, or conan.

```bash
./scripts/build.sh                 # release build, configure + build + test
./scripts/build.sh debug
./scripts/build.sh asan            # AddressSanitizer + UBSan (RelWithDebInfo)
./scripts/build.sh release --no-test
```

The script picks the host-appropriate preset (`macos-*` / `linux-*`). To drive CMake directly:

```bash
cmake --preset macos-release       # or linux-release / debug / asan
cmake --build --preset macos-release --parallel
ctest --test-dir build/macos-release --output-on-failure
./build/macos-release/pptctl info tests/fixtures/three_slides.pptx
```

Binaries land in `build/<preset>/`. The current local build preset is `macos-release`.

### Running tests

`ctest` runs the whole `pptctl_tests` doctest binary as one test named `unit`. To run a single doctest case, invoke the binary directly with a filter:

```bash
./build/macos-release/pptctl_tests --test-case="Package: read_part*"
./build/macos-release/pptctl_tests --list-test-cases
```

## Architecture

Three layers, each with one job. The separation is deliberate: `src/lib/` is designed to be extracted into a standalone `libpptctl` later, so keep CLI concerns out of it.

1. **Package layer** (`include/pptctl/package.hpp`, `src/lib/package.cpp`) — a `.pptx` is a ZIP of XML "parts" plus `_rels/` relationship files. `Package` opens the archive (minizip-ng), lists parts, and reads part bytes. It does **not** parse XML. Uses the pimpl idiom with move-only semantics.

2. **Document layer** (`include/pptctl/presentation.hpp`, `src/lib/presentation.cpp`) — semantic model on top of the package. `Presentation::load()` parses `ppt/presentation.xml` (pugixml) for slide count and canvas size, enumerates part names to count masters/layouts and summarize media, and reads theme parts for font names.

3. **Command layer** (`src/cmd/*.cpp`) — one file per subcommand, one entrypoint each. `src/main.cpp` is pure CLI11 wiring and dispatch; it holds no business logic.

Two load-bearing design choices that constrain implementations:

- **Lazy parsing.** Parts are read as bytes; XML is parsed only when a subcommand needs it. This is how the project intends to handle 1000+ slide decks without exhausting memory — don't add eager full-deck parsing.
- **Read mostly, modify surgically.** Read-only commands (info, extract, lint, diff) never round-trip. Mutating commands (optimize, merge, fill) must edit the XML tree and re-serialize **only the parts they touched** — a full "load → object model → save" round-trip corrupts parts that weren't meant to change.

### Units

OOXML's native unit is EMU (`914400` per inch; `Emu` / `kEmuPerInch` in `presentation.hpp`). Store EMU internally; convert to inches only at output.

### Adding a subcommand

1. Create `src/cmd/<name>.cpp` with an entrypoint in `namespace pptctl::cmd`.
2. Forward-declare it in `src/main.cpp` and register the CLI11 subcommand in `main()`.
3. Add the new `.cpp` to the `pptctl` executable's source list in `CMakeLists.txt`.

Adding a test file means adding `tests/test_<name>.cpp` to the `pptctl_tests` target in `CMakeLists.txt`. Fixtures live in `tests/fixtures/`; reach them via the `PPTCTL_FIXTURES_DIR` macro (set by CMake).

Some fixtures are real PowerPoint output (`minimal.pptx`, `three_slides.pptx`); others are synthetic and built by `tests/fixtures/generate_fixtures.py` (e.g. `images_deck.pptx`, `notes_and_masters.pptx`). The generator is dev-only — CMake never runs it — and is invoked via `uv` so the build stays Python-free: `uv run --python 3.12 --with python-pptx==1.0.2 --with pillow==13.0.0 tests/fixtures/generate_fixtures.py`. Edit the script and rerun it to change those fixtures; don't hand-edit the binaries.

## Conventions

- **Printing:** use the `pptctl::print` / `println` / `eprintln` helpers in `include/pptctl/print.hpp`, which wrap C++20 `std::format`. There is intentionally no `fmt` dependency, and `std::print` (C++23) is avoided to stay portable on gcc-13.
- **Formatting:** `.clang-format` (Google base, 4-space indent, 100-col limit, left pointer alignment). Run clang-format before committing.
- **Warnings are strict and apply only to project code** (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast`, etc., via the `pptctl_warnings` interface target). Third-party deps are compiled with warnings suppressed — keep that boundary; don't silence warnings in our own code by disabling the flags.
- File-local helpers go in anonymous namespaces.

## OOXML gotchas (the reason this tool exists)

- Every inter-part reference goes through `_rels/` files using relationship IDs (`rId1`, …), **scoped per part**. Naively copying a `<p:pic>` element into another deck breaks its `r:embed="rId4"` — `merge` must walk the relationship graph and rebuild references. This is why naive deck-merging tools corrupt images.
- `[Content_Types].xml` must declare every extension or explicit part path, or some renderers (Keynote especially) refuse to load the file.
- Any subcommand that writes a `.pptx` must produce output that opens **without warnings** in PowerPoint, Keynote, LibreOffice, and Google Slides. This renderer-compatibility bar is what most existing libraries fail.

See `ARCHITECTURE.md` for the full design rationale, `PLAN.md` / `ROADMAP.md` for scope and sequencing.
