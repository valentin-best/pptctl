# ROADMAP.md

A phased plan. Each phase is roughly one month of evenings/weekends for one person. Adjust to your actual pace; don't commit to dates publicly until something has shipped.

## Positioning (revised 2026-10-28)

**pptctl is static analysis and safe surgery for slide decks: `lint`, `diff`, `optimize`, `merge` — with output that opens without warnings in PowerPoint, Keynote, LibreOffice and Google Slides.** It is not a general-purpose "CLI for .pptx", and it is not an agent editing tool.

Why the change: a competitive survey in October 2026 found the general slot taken.

- **OfficeCLI** (iOfficeAI/OfficeCLI, C#, ~32k stars in seven months, weekly releases) is an agent-first editor for pptx/docx/xlsx. It covers `info` (`view stats`), `extract` (`view text`, `get --json`), template fill (its `merge` command fills `{{key}}`), and part of `lint` (`validate` = schema check, `view issues` = overflow and alt text). It has **no** diff (declined in its #359), **no** optimize, and **no** whole-deck merge. Its open bugs — slide duplication losing media rels (#534), shared tags parts PowerPoint can't open (#540), dangling chart rels (#541) — are exactly the relationship-graph failures pptctl is designed around.
- **markitdown** and **docling** own "pptx → Markdown for LLMs". `extract` is a commodity, not a selling point.
- Everything else (pptxboss, pptx-review, pptx-cli, pptxdiff, puppt, hands-on-deck, pptx-downsizer) is small or abandoned. No maintained, headless, CI-friendly tool does diff, optimize or a corruption-free merge.

Consequences for the plan below: `optimize` and `merge` stay; `lint` gains package-integrity rules so pptctl can verify what *other* tools write; `fill` is deferred (OfficeCLI covers it); Phase 7 becomes integrations (CI, git, agent skill); library extraction waits for demand.

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

## Phase 4 — `pptctl diff` (weeks 11–14) ✅

Goal: the feature no OSS tool does well.

- [x] Per-slide identity: hash + best-match heuristic for reordered slides.
- [x] Text-level diff within slides.
- [x] Shape-level diff (moved, resized, recolored).
- [x] Picture diff (added, removed, replaced; not pixel-diff).
- [x] Output formats: unified-text (for terminal), JSON (for tools), HTML (for review).
- [x] Git integration: `git config diff.pptx.textconv "pptctl diff --unified"` example in docs.

Exit criteria: diffing two real-world decks where you made known small edits produces a diff that *matches* what you changed.

**Ship moment:** v0.4.0. Pitch to legal/finance communities — they care most.

## Phase 5 — `pptctl optimize` (weeks 15–17) ✅

Goal: make 50–90% file size reductions one command.

- [x] Recompress embedded JPEGs/PNGs at configurable quality.
- [x] Remove unused slide masters and layouts.
- [x] Strip embedded fonts (with `--keep-fonts` opt-out).
- [x] Strip cropped portions of images (PowerPoint stores the full image even after a crop).
- [x] Remove orphan media (files in `ppt/media/` with no relationships).
- [x] Report what was changed.

Exit criteria: on a corpus of ten public decks, average size reduction ≥40% with no visible quality loss in PowerPoint and Keynote.

Outcome: met on the full 15-deck public corpus (42.9% mean, median 43.4%, no visible quality loss), but not uniformly: the 10 university lecture decks alone average 35.8%, held back by vector metafiles, OLE embeddings and screenshots already at their displayed size. See `docs/phase5-optimize-validation.md`.

**Ship moment:** v0.5.0. Nobody else does this well, and the story is concrete: "shrinks decks 40–90% without breaking them in Keynote". A natural candidate for going public (still the maintainer's call).

## Phase 6 — `pptctl merge` + package integrity (weeks 18–21) ✅

The one with the most subtle bugs. Save it for when you have your sea legs.

Package-integrity lint rules come first: they are the test oracle for `merge`, and they make `pptctl lint` the verifier for decks written by other tools (OfficeCLI, python-pptx, agents).

- [x] Lint: **dangling relationship** — a rel whose internal target part does not exist.
- [x] Lint: **shared part** — one part targeted from two places that must own it exclusively (tags, notes, charts' style/color parts; see OfficeCLI #540, #541).
- [x] Lint: **undeclared content type** — a part with neither a `Default` extension nor an `Override` in `[Content_Types].xml`.
- [x] Lint: **orphan part** — a part no relationship reaches (also feeds `optimize`).
- [x] Lint: **broken r:id** — an `r:embed` / `r:id` / `r:link` in slide XML that the part's `.rels` does not define.
- [x] Concatenate slides from multiple decks.
- [x] Rewrite all relationship IDs.
- [x] De-duplicate media (same image hash → single file).
- [x] Theme merge strategy (use first deck's theme, or `--theme=N` to pick).
- [x] Slide master de-duplication.
- [x] Lint: **unlisted part** — a slide or master related but left out of its list (found while building `merge`: Keynote refuses python-pptx decks with notes).

Exit criteria: merge ten 5-slide decks; output passes `pptctl lint --strict` on the integrity rules and opens in PowerPoint, Keynote, LibreOffice, and Google Slides with all images intact.

Outcome: met. Ten decks from six producers merge into a 75-slide deck that passes every integrity rule and opens in all four renderers, with 73 of 75 slides pixel-identical to their sources in each. The differences are one fidelity gap (#163, text inheriting the base deck's default text style) and renderers that read one theme for the whole deck (Keynote hyperlinks, Google Slides charts). See `docs/phase6-merge-validation.md`.

**Ship moment:** v0.6.0. "Merge decks without corrupting them" — the headline feature.

## Phase 7 — integrations (weeks 22–24)

Meet users where decks already flow: pull requests, git, and agent pipelines. Distribution, not new subcommands.

- [x] GitHub Action: `lint` and `diff` decks changed in a PR, post the diff as a job summary.
- [x] pre-commit hook definition (`.pre-commit-hooks.yaml`).
- [x] Git setup docs/command for `diff.pptx.textconv` and a difftool.
- [x] Agent skill file (`SKILL.md`): "after generating a deck, run `pptctl lint --strict`". A usage guide for agents, not an AI subcommand.
- [x] Renderer-compatibility corpus: public decks + CI job that opens `optimize`/`merge` output headlessly in LibreOffice; published results table.

Exit criteria: a sample repo where a PR touching a `.pptx` gets lint findings and a readable diff with no manual setup beyond adding the Action.

**Ship moment:** v0.7.0. "Code review for slide decks."

## Deferred (demand-triggered)

Not scheduled. Pick up only when real users ask.

- **`pptctl fill`** — `{{key}}` templating is already covered by OfficeCLI's `merge`. The parts it lacks (repeat-slide `{{#each}}`, image substitution) are the only reason to revisit. The CLI stub stays.
- **Library extraction** — move `include/pptctl/` into `libpptctl`, Python binding (pybind11), Node binding (N-API), documented C/C++ APIs. OfficeCLI and pptxboss already ship SDKs; a pptctl binding is worth it once people want diff/optimize/merge inside their own pipelines. Keep `src/lib/` free of CLI concerns so this stays cheap.

## Anti-roadmap (things to refuse for at least year 1)

- A web UI.
- A SaaS hosted version.
- DOCX / XLSX support. (Tempting because OOXML overlap is huge. But scope discipline matters more.)
- An AI-powered subcommand. (Plenty of projects do that. Yours is the layer underneath them.)
- A general-purpose deck editor (`set`/`add`/`move` shape-level edits for agents). That is OfficeCLI's ground; pptctl verifies and repairs what editors produce.
- A configuration file system.
- A plugin system.
- Custom rendering (PNG/PDF output beyond shelling out to LibreOffice).

Whenever someone files an issue asking for these, link them here.
