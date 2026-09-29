# pptctl

> A fast CLI for `.pptx` files. Inspect, lint, diff, optimize, merge, extract.

**Status:** early development, v0.3.0. `info`, `extract` and `lint` work; the others are stubs.

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
| `diff` | Semantic diff: slides added/removed, text changed, charts modified | 🚧 In progress (slide-level) |
| `merge` | Merge decks while preserving images, themes, and slide masters | Planned |
| `fill` | Template-fill from a `.pptx` template + JSON data | Planned |

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
