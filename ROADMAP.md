# ROADMAP.md

A phased plan. Each phase is roughly one month of evenings/weekends for one person. Adjust to your actual pace; don't commit to dates publicly until something has shipped.

## Phase 0 — Foundation (weeks 1–2) ✅

Goal: skeleton you can build on. No features yet.

- [x] Pick final project name. Update everywhere.
- [x] Pick license. Add `LICENSE`.
- [x] CMake project: builds an empty `pptctl` binary with `--version` and `--help`.
- [x] `FetchContent` for CLI11, pugixml, minizip-ng, fmt, nlohmann/json, doctest.
- [x] First subcommand wired up: `pptctl hello` that prints "hello, world". Proves the dispatch.
- [x] GitHub Actions CI: build on Linux, macOS, Windows. Run any tests that exist.
- [x] `.editorconfig`, `.clang-format`, `.clang-tidy` configs.
- [x] First fixture: hand-craft `tests/fixtures/minimal.pptx` (one blank slide).
- [x] First test: open the fixture, verify ZIP listing matches expectations.

Exit criteria: clean CI on all three OSes, one fixture round-trips losslessly. ✅

## Phase 1 — `pptctl info` (weeks 3–4) ✅

Goal: ship the smallest useful subcommand. This is the moment you have something to post.

- [x] OOXML package layer: open a `.pptx`, list parts, resolve `[Content_Types].xml`.
- [x] Read `ppt/presentation.xml`: slide dimensions, slide list.
- [x] Read theme: font references, color scheme.
- [x] Walk `ppt/media/`: count and sum sizes of images, video, audio.
- [x] `pptctl info <file>` output formatted nicely (see README for shape).
- [x] `--json` flag for machine-readable output.
- [x] Fixtures: minimal deck, a 12-slide deck with images, a deck with notes and masters.

Exit criteria: `pptctl info` works on ten real-world `.pptx` files from the wild without crashing. ✅

**Ship moment:** tag v0.1.0, write a short blog post or README update, post to r/programming and HN with title like "pptctl: a CLI for inspecting .pptx files". Don't oversell — say it's early.

## Phase 2 — `pptctl extract` (weeks 5–7) ✅

Goal: structured content extraction. The "Docling for presentations" piece.

- [x] Slide content parsing: shape tree, text frames, paragraphs, runs.
- [x] Picture extraction (with relationship resolution).
- [x] Table extraction.
- [x] Chart metadata (type, series count, data) — values only, not rendering.
- [x] Speaker notes.
- [x] `--format json` (default), `--format md`, `--format txt`.
- [x] Schema spec for JSON output, versioned. (See `docs/extract-schema.md`.)
- [x] Golden tests: every fixture has an expected-output file checked in.

Exit criteria: `pptctl extract` produces consistent output across ten real-world decks. Output is good enough that an LLM could read it and reason about the deck. ✅

**Ship moment:** v0.2.0. Now there's a story: pptctl is the OSS tool for getting structured data out of presentations. Pitch on r/MachineLearning, r/LocalLLaMA — people want this for RAG pipelines.

## Phase 3 — `pptctl lint` (weeks 8–10) ✅

Goal: catch common authoring problems statically.

Lint rules to implement (start with the most universal):

- [x] **Canvas overflow**: any shape extending past slide bounds.
- [x] **Tiny text**: font size below a threshold (default 12pt).
- [x] **Missing alt-text on images**: accessibility.
- [x] **Font fallback risk**: font referenced but no embed and not in safe-list.
- [x] **Empty placeholders**: layouts with placeholders that have no content.
- [x] **Suspect content types**: e.g. printer-settings stubs that break Keynote (anthropics/skills#1167).
- [x] **Image bloat**: images stored at much higher resolution than displayed.
- [x] Output: human-readable by default; `--json` for machine; `--strict` for non-zero exit on any warning.

Exit criteria: lint catches at least one real issue in 50% of real-world decks from a sample of ten.

**Ship moment:** v0.3.0. This is the moment for a "static analysis for your slide decks" post.

## Phase 4 — `pptctl diff` (weeks 11–14)

Goal: the feature no OSS tool does well.

- [ ] Per-slide identity: hash + best-match heuristic for reordered slides.
- [ ] Text-level diff within slides.
- [ ] Shape-level diff (moved, resized, recolored).
- [ ] Picture diff (added, removed, replaced; not pixel-diff).
- [ ] Output formats: unified-text (for terminal), JSON (for tools), HTML (for review).
- [ ] Git integration: `git config diff.pptx.textconv "pptctl diff --unified"` example in docs.

Exit criteria: diffing two real-world decks where you made known small edits produces a diff that *matches* what you changed.

**Ship moment:** v0.4.0. Pitch to legal/finance communities — they care most.

## Phase 5 — `pptctl optimize` (weeks 15–17)

Goal: make 50–90% file size reductions one command.

- [ ] Recompress embedded JPEGs/PNGs at configurable quality.
- [ ] Remove unused slide masters and layouts.
- [ ] Strip embedded fonts (with `--keep-fonts` opt-out).
- [ ] Strip cropped portions of images (PowerPoint stores the full image even after a crop).
- [ ] Remove orphan media (files in `ppt/media/` with no relationships).
- [ ] Report what was changed.

Exit criteria: on a corpus of ten public decks, average size reduction ≥40% with no visible quality loss in PowerPoint and Keynote.

## Phase 6 — `pptctl merge` (weeks 18–20)

The one with the most subtle bugs. Save it for when you have your sea legs.

- [ ] Concatenate slides from multiple decks.
- [ ] Rewrite all relationship IDs.
- [ ] De-duplicate media (same image hash → single file).
- [ ] Theme merge strategy (use first deck's theme, or `--theme=N` to pick).
- [ ] Slide master de-duplication.

Exit criteria: merge ten 5-slide decks; output opens in PowerPoint, Keynote, LibreOffice, and Google Slides with all images intact.

## Phase 7 — `pptctl fill` (weeks 21–24)

Templating without the Jinja-in-a-binary-file horror.

- [ ] Define template marker syntax (e.g. `{{customer.name}}` inside text runs).
- [ ] Walk text runs, substitute markers from JSON data.
- [ ] Repeat-slide syntax: `{{#each items}}` style for generating slides from arrays.
- [ ] Image substitution: `{{image:logo}}` swaps a placeholder picture for one from data.

Exit criteria: produce ten personalized decks from one template + ten JSON files; visual diff between them shows only the intended changes.

## Phase 8 onwards — extraction into a library

By month ~6, the parser and document model should be stable enough to extract:

- [ ] Move `include/pptctl/` into a separate repo: `libpptctl`.
- [ ] CLI consumes it as a dependency.
- [ ] First binding: Python via pybind11.
- [ ] Second binding: Node via N-API or Neon (if Rust).
- [ ] Document the C and C++ APIs separately.

This is when the project graduates from "neat CLI" to "infrastructure people depend on." It's also when contributors start showing up for real.

## Anti-roadmap (things to refuse for at least year 1)

- A web UI.
- A SaaS hosted version.
- DOCX / XLSX support. (Tempting because OOXML overlap is huge. But scope discipline matters more.)
- An AI-powered subcommand. (Plenty of projects do that. Yours is the layer underneath them.)
- A configuration file system.
- A plugin system.
- Custom rendering (PNG/PDF output beyond shelling out to LibreOffice).

Whenever someone files an issue asking for these, link them here.
