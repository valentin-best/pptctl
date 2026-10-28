# PLAN.md — pptctl

> Working name. Easy to rename with one sed. Other candidates: `decktool`, `pptkit`, `oox`, `slidesmith`.

## Vision

A fast, single-binary CLI for `.pptx` files, written in C++. Each subcommand attacks one concrete pain point that current tooling (python-pptx, PptxGenJS, Apache POI) handles badly or not at all. The underlying OOXML parser/serializer is designed from day one to be extracted into a reusable library with bindings to Python / JS / Rust once the CLI has validated the API.

Think `ripgrep` for grep, `bat` for cat, `fd` for find — but for `.pptx`.

> **Positioning update (2026-10-28).** Since this plan was written, OfficeCLI (agent-first pptx/docx/xlsx editor, ~32k stars) took the "general CLI for Office files" slot, and markitdown/docling own pptx → Markdown extraction. pptctl now narrows to what none of them do: **lint, diff, optimize and a corruption-free merge**, run in CI and pipelines on decks other tools produce. `fill` and library extraction are deferred until users ask. See the Positioning section of ROADMAP.md for the evidence.

## The problem (what hurts, with receipts)

### Library-level pain (python-pptx)

| Pain | Evidence |
|---|---|
| Animation support is absent | scanny/python-pptx #102 (2014), #264, #400, #1106 — open eleven years |
| SmartArt cannot be created or modified | scanny/python-pptx #83, #448 |
| Modern chart types missing (Waterfall, Treemap, Sunburst, Box & Whisker, Histogram, Pareto, 3D) | scanny/python-pptx #583, #266 |
| `chart.replace_data()` is fragile and type-restricted | SlideForge production postmortem |
| Memory degrades past ~1000 slides | SlideForge: 1.7GB RSS on a 323-slide deck |
| Round-trips silently strip SmartArt, custom XML, embedded videos | Same |
| No PDF export | Documented limitation |
| Font references are written without validation; silent substitution at render time | Documented limitation |
| OOXML output not always renderer-portable | anthropics/skills #1167: default template ships a Windows printer-settings stub that breaks Keynote |

### Workflow-level pain (any PPTX tool)

| Pain | Evidence |
|---|---|
| Naive deck merging loses embedded pictures (relationship IDs not rewritten) | SlideForge: 80-line workaround |
| No semantic diff between two `.pptx` files | scanny/python-pptx #637; HN threads from legal/finance industry |
| No bloat optimization CLI (image recompression, unused-master removal, font stripping) | Industry guides recommend manual clicking; 50–90% reductions are possible |
| No layout/canvas validator | SlideForge ships a 46-rule linter internally; dropped broken-slide rate from 12% to <1% |
| No accessibility / brand-compliance linter | No existing tool |
| Template-fill tools are library-specific and verbose | python-pptx-template, docxtpl-style: each language reinvents this |

### Ecosystem-level pain (OOXML at large)

- No clean, modern, **cross-language** library for OOXML. Each ecosystem reimplements the wheel badly.
- C++ specifically has nothing comparable to python-pptx — Aspose is closed and ~$3K/dev/yr; Apache POI is Java; libreoffice is too heavy to embed.

## Solution — what we're building

A single static binary, `pptctl`, with subcommands that each solve one of the pain points above. Designed so the parsing/serialization core is reusable for a future library.

### v0 subcommands (initial scope)

```
pptctl extract <file.pptx>          # → structured JSON/markdown of slide content
pptctl info <file.pptx>             # quick metadata (slide count, size, masters, fonts, media)
pptctl lint <file.pptx>             # canvas overflow, missing alt-text, font fallback risk
pptctl optimize <file.pptx>         # recompress images, strip unused masters/fonts
pptctl diff a.pptx b.pptx           # semantic diff: slide added/removed, text changed, chart updated
pptctl merge a.pptx b.pptx -o out   # merge that actually preserves images and themes
pptctl fill template.pptx --data data.json   # safe templating; no Jinja required (deferred)
```

These are not all v0.1 — see ROADMAP.md for phasing.

### Non-scope (intentional)

- **Rendering**: we do not draw slides to PNG/PDF ourselves. For PDF export, we can shell out to LibreOffice (documented), but pptctl itself stays focused on file-level manipulation.
- **A presentation runtime**: not Reveal.js, not Slidev. We don't *play* slides.
- **A GUI**: never. CLI only. A library and bindings later.
- **DOCX and XLSX**: same format family (OOXML), and the parsing infrastructure will be reusable, but the v1 product is PPTX-only. DOCX/XLSX become future projects that share the core lib.
- **AI generation**: we are not an LLM wrapper. Presenton, SlideForge, Gamma do that. We're the layer underneath them — what they should be using as a foundation.

## Why this is the right shape for a first OSS project

- **Each subcommand is shippable independently.** `extract` alone is a useful PyPI/Hacker-News-worthy moment. Then `lint`. Then `diff`. Three launches, not one.
- **Solo-feasible.** Scope is bounded by a known file format. No machine learning, no design problems, no UI work.
- **The hard part is the foundation, and it's reusable.** A solid OOXML parser/serializer in modern C++ becomes the basis for a library later — that's the long-term value capture.
- **C++ is genuinely the right tool** for binary file parsing, single-static-binary distribution, and future native bindings. (See ARCHITECTURE.md for tradeoff vs. Rust.)
- **The pain is real and well-documented.** No need to evangelize the problem — every developer who has touched python-pptx has felt it.

## Success metrics (what good looks like)

**Year 1:**
- `pptctl extract` and `pptctl info` shipped. Static binaries for macOS (arm64+amd64), Linux (amd64+arm64), Windows.
- 500+ GitHub stars.
- 2–3 unsolicited blog posts / HN front-page appearance.
- At least one downstream user — someone using pptctl in a CI pipeline or product.

**Year 2:**
- All v0 subcommands shipped (except `fill`, deferred).
- pptctl running in other people's CI via the GitHub Action / pre-commit hook.
- Core extracted into `libpptctl` with Python bindings via pybind11 — if users ask for it.
- 2k+ stars. Real contributors (not just drive-by PRs).

**Year 3 — the bet:**
- The library has Python, JS, Rust, Go bindings. It's the *default* OOXML library people reach for outside .NET/Java.
- DOCX/XLSX siblings live alongside.

If years 1 or 2 don't happen, the project hasn't worked — pivot or wind down honestly.

## Decisions to make before starting

1. **Final project name.** `pptctl` works; if it doesn't sing, decide now and rename once.
2. **License.** Recommendation: **MIT** (maximal adoption). Apache-2.0 is fine if patent grant matters to you. Avoid GPL — it'll deter library users later.
3. **C++ or Rust.** Honestly, Rust is the better language for this in 2026 (cargo, memory safety, easier cross-platform, easier WASM). C++ is workable; pick it if you care about *learning* C++ or about embedding in C++ ecosystems. See ARCHITECTURE.md for the tradeoff.
4. **Where to host.** GitHub default. Set up CI (GitHub Actions) before commit one.
5. **Whether to dual-license** core lib (e.g. AGPL for the binary, MIT for the library) — usually not worth it for a solo first project.

## First 30 days

See ROADMAP.md for the full phased plan. TL;DR for the first month:

- Week 1: project scaffolding, CMake, CI, hello-world binary, first issue templates.
- Week 2: ZIP + XML core. Parse a `.pptx` into an internal tree representation.
- Week 3: `pptctl info` — slide count, dimensions, fonts referenced, media list. Cheapest useful command.
- Week 4: `pptctl extract` — JSON output of slide text and layout. First proper post-worthy moment.

After that, you've validated the foundation and you can decide whether to keep going.

## Risks and honest concerns

- **OOXML is huge.** The full ECMA-376 spec is 6000+ pages. Most of it doesn't matter for v0, but corners will bite. Mitigation: scope hard, fail loud, defer features.
- **One-person OSS projects burn out.** Mitigation: ship narrow, ship often, don't promise a roadmap publicly until you've shipped something.
- **Competing with paid tools (Aspose).** They have feature parity we won't match for years. Don't try; pick the workflows they're worst at (CLI, scripting, CI).
- **"Just use Aspose / SlideForge / Presenton."** Real answer to skeptics: those don't solve the underlying problem of bad OSS libraries — they sell hosted services. We're building the foundation, not another wrapper.
