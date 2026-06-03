# ARCHITECTURE.md

How `pptctl` is put together and why. Audience: anyone (including future you) implementing or extending the code.

## Language choice: C++ vs Rust — be honest with yourself

You said you prefer C++. For a CLI that does zip+XML manipulation, both are good choices. The honest tradeoff:

**Rust is probably the better technical choice in 2026:**
- `cargo` makes dependency management painless vs CMake's ceremony.
- Memory safety eliminates an entire class of bugs that *will* show up when parsing untrusted XML/ZIP from arbitrary `.pptx` files in the wild.
- Cross-compilation to macOS/Linux/Windows is one line (`cross build`).
- WASM compilation is straightforward — useful later if you want browser-side parsing.
- The library ecosystem is excellent: `zip`, `quick-xml`, `clap`, `serde_json`.
- Examples of OSS CLI tools written by solo devs that broke through: ripgrep, fd, bat, eza, hyperfine, zoxide, ast-grep, ruff, uv. The majority are Rust.

**C++ is fine and is the right pick if:**
- You actively want to write/learn modern C++ — this is a legitimate reason.
- You eventually want C/C++ ecosystem embedding (LibreOffice, Adobe products, games).
- You already have the toolchain mental model.

This document assumes C++. If you reread this in a week and Rust feels right, switch — sooner is cheaper.

## High-level structure

```
pptctl/
├── CMakeLists.txt          # top-level build
├── src/
│   ├── main.cpp            # CLI11 dispatch
│   └── cmd/                # one file per subcommand
│       ├── info.cpp
│       ├── extract.cpp
│       ├── lint.cpp
│       └── ...
├── include/pptctl/
│   ├── package.hpp         # OOXML package model (ZIP + parts + relationships)
│   ├── presentation.hpp    # presentation-level abstractions (slides, masters, layouts)
│   ├── slide.hpp           # slide content (shapes, text, pictures)
│   ├── xml.hpp             # pugixml wrappers
│   └── zip.hpp             # minizip-ng wrappers
├── tests/
│   ├── fixtures/           # small .pptx files for testing
│   └── *.cpp               # doctest cases
└── docs/
```

The clear separation is intentional: today this is a CLI that uses an internal library (`include/pptctl/`). Tomorrow, that library gets extracted into its own repo and the CLI consumes it as a dependency. Designing this way from day one means the library extraction is mechanical, not a rewrite.

## Library boundaries

There are three layers, each with one job:

### 1. Package layer (`package.hpp`, `zip.hpp`)

A `.pptx` is a ZIP archive containing XML "parts" plus `[Content_Types].xml` plus `_rels/` relationship files. This layer:
- Opens a `.pptx` as a stream of parts (name → bytes).
- Resolves relationships ("which slide does this reference?").
- Writes a package back to disk without re-serializing parts that weren't touched (crucial for avoiding round-trip data loss — see PLAN.md).

**Key design choice:** **lazy parsing**. We do *not* parse all XML on load. Parts are read into a byte buffer; only when a subcommand asks for them do we parse with pugixml. This is how we'll handle 1000+ slide decks without blowing memory.

### 2. Document layer (`presentation.hpp`, `slide.hpp`)

The semantic model:
- `Presentation` — slide masters, layouts, theme, slide list.
- `Slide` — shape tree, notes slide, layout reference.
- `Shape` — text frame, picture, chart, group, placeholder, etc.

**Key design choice:** **read mostly, modify surgically**. Most subcommands (info, extract, lint, diff) are read-only. The few that modify (optimize, merge, fill) operate on the XML tree directly and re-serialize only the parts they touched. We never do a "load → object model → save" round-trip for parts we didn't intend to change.

### 3. Command layer (`src/cmd/`)

Each subcommand is one file with one entrypoint:

```cpp
int cmd_info(const InfoArgs& args);
int cmd_extract(const ExtractArgs& args);
// ...
```

`main.cpp` is just CLI11 wiring and dispatch. No business logic.

## Dependencies (and why these)

All pulled in via CMake `FetchContent` — single `cmake --preset linux-release` or `./scripts/build.sh` and you're going. No vcpkg, no conan, no system packages.

| Dep | Version | Why |
|---|---|---|
| CLI11 | v2.5.0 | Modern, header-only, ergonomic. Better than argparse-like wrappers. |
| pugixml | v1.15 | Small, fast, well-documented, MIT. The C++ XML library. |
| minizip-ng | 4.0.10 | Modern fork of minizip with better API. Used by Blender, Krita, etc. |
| nlohmann/json | v3.12.0 | The de-facto C++ JSON. Header-only. |
| doctest | v2.4.12 | Lighter than Catch2, faster to compile, good enough. |

**Notably not in the list: `fmt`.** We use C++20 `std::format` from the standard library through a tiny header (`include/pptctl/print.hpp`). Same `{}` placeholder syntax, no third-party dependency, no `consteval` breakage when toolchains update. fmt is great but the standard library version of its core API is now ubiquitous.

**Excluded on purpose:**
- **Boost** — too heavy for what we need. We can pull individual headers if necessary.
- **LibreOffice / UNO** — embedding a full office suite is the opposite of what we want.
- **fmt** — superseded by `std::format` (see above).

## OOXML basics (for future you, who will forget)

A `.pptx` is a ZIP. Unzip one and you get something like:

```
[Content_Types].xml          # MIME-type map for parts
_rels/.rels                  # top-level relationships
docProps/
  app.xml                    # author, app version, etc.
  core.xml                   # title, created/modified dates
ppt/
  presentation.xml           # slide list, dimensions, default text styles
  _rels/presentation.xml.rels # which slide files this references
  slideMasters/
    slideMaster1.xml         # master template
    _rels/slideMaster1.xml.rels
  slideLayouts/
    slideLayout1.xml
    ...
  slides/
    slide1.xml               # actual slide content
    _rels/slide1.xml.rels
    ...
  theme/
    theme1.xml               # colors, fonts
  media/
    image1.png               # embedded images
  charts/
    chart1.xml               # if any
```

Key invariants:
- Every reference between parts goes through `_rels/` files using **relationship IDs** (`rId1`, `rId2`, …). These IDs are scoped per part.
- The same physical image can be referenced by multiple slides via separate relationships.
- `[Content_Types].xml` must list every file extension or explicit part path. If it doesn't, some renderers (Keynote, in particular) refuse to load.

This relationship-graph nature is *why* naive deck merging corrupts pictures: copying a `<p:pic>` XML element to a new deck means its `r:embed="rId4"` no longer points to anything. `pptctl merge` has to walk the graph and rebuild references.

## Testing strategy

- **Unit tests** (doctest) live next to source files: parsing helpers, relationship resolution, etc.
- **Fixture tests**: `tests/fixtures/` contains small hand-crafted `.pptx` files. Each fixture exercises one edge case: a deck with one slide, with notes, with a chart, with SmartArt (we should *preserve* it, not modify it), with embedded media, with custom XML parts.
- **Golden tests** for `extract` and `info`: input fixture → expected JSON output, byte-compared after canonicalization.
- **Renderer compatibility tests** (manual at first, automated eventually): for any subcommand that produces a `.pptx`, the output must open without warnings in PowerPoint, Keynote, LibreOffice, and Google Slides. This is the gotcha most libraries fail. See anthropics/skills#1167 for an example of what a passing test should catch.

## CI matrix

GitHub Actions, three jobs (all green):
- `ubuntu-latest` (gcc-13 and clang-18)
- `macos-latest` (Apple Silicon)
- `windows-latest` (MSVC 2022)

Each runs: build → unit tests → fixture tests → CLI smoke tests.

Static analysis: `clang-tidy` runs as a pre-commit hook and in CI on every PR. Memory: `-fsanitize=address,undefined` via `./scripts/build.sh asan`.

## Distribution

- **Release artifacts**: prebuilt static binaries per platform, attached to GitHub Releases. Use `actions/upload-release-asset`.
- **Homebrew tap**: create one once v0.2 ships. `brew install YOUR-USERNAME/tap/pptctl`.
- **Cargo crates / PyPI / npm**: not yet. Those land when the core extracts into a library.
- **Docker**: only if there's demand. The whole point is a static binary that doesn't need it.

## What we're *not* doing in the architecture

- **No threads in v0.** Single-threaded is fast enough for files up to a few GB.
- **No async I/O.** This is a CLI, not a server.
- **No plugins.** Subcommands are statically linked. Plugin systems are a productivity tax in early projects.
- **No config file.** All options are CLI flags. Fewer concepts for users.
- **No state across invocations.** Every run is independent. No caches, no daemon.

## Open architectural questions

1. **How do we represent shape position/size in our document model?** Native units are EMU (914400 per inch). Inches are user-friendly. Probably: store EMU internally, render as inches in JSON output.
2. **Do we ever load an entire deck into memory?** Goal is to support 10k-slide decks. Probably needs a streaming mode for `info` and `extract`.
3. ~~**Schema for `extract` JSON output.**~~ Resolved: versioned JSON schema spec lives at `docs/extract-schema.md`. Breaking changes increment `schema_version`.
4. **`diff` output format.** Unified diff style? JSON patch? Structured tree diff? Decide before implementation — affects the entire shape of the subcommand.
