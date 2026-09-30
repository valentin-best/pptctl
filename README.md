# pptctl

> A fast CLI for `.pptx` files. Inspect, lint, diff, optimize, merge, extract.

**Status:** early development, v0.3.0. `info`, `extract` and `lint` work; `diff` works and is still growing (v0.4.0 in progress); the others are stubs.

```
$ pptctl info deck.pptx
file                deck.pptx  (1.4 MiB)
slides              12
canvas              13.33in × 7.50in
masters             2
layouts             11
fonts referenced    Calibri, Inter, JetBrains Mono
media               7 images (1.1 MiB), 0 video, 0 audio
parts in package    58
```

`--json` mode emits the same data machine-readable.

```
$ pptctl extract deck.pptx --format txt
slide 1
  [ctrTitle] Q3 Business Review
  [body] Revenue up 23% YoY
  [pic] chart_bg.png  (1024×768, 184 KiB)
slide 2
  [title] Agenda
  [body] Intro\nDemo\nQ&A
...
```

`--format json` (default) emits a versioned schema suitable for LLM pipelines and tooling. `--format md` emits Markdown.

## Why this exists

The dominant libraries for working with `.pptx` files programmatically — python-pptx, PptxGenJS, Apache POI — were each designed for a specific job and have not kept up with what people actually do with presentations:

- `python-pptx` has had **open issues for animation support since 2014**, no SmartArt, no modern chart types, no PDF export, and degrades past ~1000 slides.
- No tool in the open-source ecosystem can semantically **diff** two decks, **merge** them without corrupting embedded images, or **lint** them for layout problems.
- The closed-source alternative (Aspose) costs $3K+/developer/year.

`pptctl` is a CLI written in modern C++ that handles the workflows existing tools don't, and is designed so its parser/serializer becomes a reusable library with cross-language bindings.

See [PLAN.md](./PLAN.md) for the long version.

## Planned subcommands

| Command | What it does | Status |
|---|---|---|
| `info` | Quick metadata: slide count, dimensions, fonts, media | ✅ Implemented |
| `extract` | Structured JSON/markdown export of slide content | ✅ Implemented |
| `lint` | Static analysis: overflow, tiny text, alt-text, font risk, empty placeholders, content types, image bloat; `--json`, `--strict` | ✅ Implemented |
| `optimize` | Recompress images, strip unused masters/fonts (50–90% size cuts) | Planned |
| `diff` | Semantic diff: slides added/removed/moved/changed, text, shape and picture ops; `--format unified\|json\|html`, `--quiet`, exit codes like `git diff` | ✅ Implemented (v0.4.0 in progress: real-world validation) |
| `merge` | Merge decks while preserving images, themes, and slide masters | Planned |
| `fill` | Template-fill from a `.pptx` template + JSON data | Planned |

## `pptctl diff`

Semantic diff between two decks. Slides are matched by content, not by position: an identical slide is found again after a reorder and reported as moved, an edited slide is paired with its most similar counterpart, and only what is left over is reported as added or removed. Inside each changed slide the diff lists text ops (paragraphs inserted, deleted, changed, reformatted), shape ops (added, removed, moved, resized, recolored) and picture ops (added, removed, replaced, re-linked, alt text changed).

```
$ pptctl diff deck.pptx deck-v2.pptx
--- deck.pptx
+++ deck-v2.pptx
@@ slide 3 -> 1 (moved) @@
@@ slide 2 -> 3 (changed 0.50) @@
-Slide 2
+Slide 2: revised title
+New subtitle
@@ slide 4 (added) @@
4 slides: 1 added, 0 removed, 1 moved, 1 changed
```

One `@@` hunk per slide that is not simply unchanged; the number after `changed` is the similarity that paired the two slides. Text edits are `-`/`+` lines, run reformatting is a `~` line, and shape and picture ops are indented one-liners:

```
$ pptctl diff images.pptx images-v2.pptx
--- images.pptx
+++ images-v2.pptx
@@ slide 3 -> 3 (changed 0.67) @@
  picture "Picture 2" replaced (image3.png 127 B 66x50 -> picture3.png 121 B 66x50)
@@ slide 5 -> 5 (changed 0.80) @@
  shape "Picture 2" (picture) removed
  picture "Picture 2" removed (image5.png 128 B 68x52)
@@ slide 7 -> 7 (changed 0.86) @@
  shape "Picture 3" (picture) added at (5.00 in, 1.50 in), 4.00 x 3.00 in
  picture "Picture 3" added (picture7.png 170 B 96x72)
…
12 slides: 0 added, 0 removed, 0 moved, 4 changed
```

Three formats: `--format unified` (default, shown above; coloured on a TTY, `--color`/`--no-color` to force it, `NO_COLOR` honoured), `--format json` (the full slide mapping including unchanged slides, versioned; see [docs/diff-schema.md](./docs/diff-schema.md)) and `--format html` (one self-contained page to hand to a reviewer). Identical decks print nothing in unified mode.

| Flag | Meaning |
|---|---|
| `--match-threshold F` | Minimum similarity (0-1, default 0.5) to pair two edited slides instead of reporting one removed and one added. |
| `--strict-text` | Also report whitespace-only changes and field-run (`a:fld`) churn, which are ignored by default. |
| `--geometry-tolerance N` | Ignore shape moves and resizes of at most N EMU (default 9525, one pixel at 96 DPI; 0 reports everything). |
| `-q`, `--quiet` | Print nothing; exit code only. |

Exit codes follow `git diff`: `0` identical, `1` differences found, `2` error (a file could not be opened or is not a `.pptx`). Handy in CI:

```bash
pptctl diff -q approved.pptx candidate.pptx || echo "deck changed"
```

To get these diffs from `git diff` and `git log -p`, see [docs/git-integration.md](./docs/git-integration.md).

Not there yet (Phase 4 is still open): table and chart contents are not compared, and the diff has not been validated on a corpus of real-world decks.

## Building

Requires **CMake ≥ 3.24**, **Ninja**, and a **C++20 compiler with `std::format`**:

- Apple Clang ≥ 16 (Xcode 16+)
- LLVM Clang ≥ 17 (with libc++ ≥ 17 or libstdc++ ≥ 13)
- GCC ≥ 13
- MSVC ≥ 19.30

### Quick start (macOS or Linux)

```bash
# Install tooling once
brew install cmake ninja                    # macOS
sudo apt install cmake ninja-build          # Debian/Ubuntu

# Build, test, run
./scripts/build.sh                          # release build
./scripts/build.sh debug
./scripts/build.sh asan                     # AddressSanitizer + UBSan
./scripts/build.sh release --no-test        # skip tests
```

### Or with CMake directly

```bash
cmake --preset macos-release                # or linux-release
cmake --build --preset macos-release --parallel
ctest --test-dir build/macos-release --output-on-failure
./build/macos-release/pptctl --version
```

Dependencies are fetched automatically via CMake `FetchContent`:

- [CLI11](https://github.com/CLIUtils/CLI11) v2.5.0 — argument parsing
- [pugixml](https://github.com/zeux/pugixml) v1.15 — XML parser
- [minizip-ng](https://github.com/zlib-ng/minizip-ng) 4.0.10 — ZIP I/O
- [nlohmann/json](https://github.com/nlohmann/json) v3.12.0 — JSON output
- [doctest](https://github.com/doctest/doctest) v2.4.12 — testing

No `fmt` — we use C++20 `std::format` directly.

## Contributing

Not accepting feature PRs yet — the API is in too much flux. Bug reports, design feedback, and "I wish pptctl could…" issues are very welcome.

## License

MIT. See [LICENSE](./LICENSE).
