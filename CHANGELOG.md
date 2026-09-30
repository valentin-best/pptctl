# Changelog

All notable changes to `pptctl` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each version corresponds to a
ROADMAP phase and a GitHub milestone.

## [0.4.0] — 2026-09-30

Phase 4: `pptctl diff`, semantic diff between two decks.

### Added

- Table and chart diff (#74): tables on a matched slide pair are paired by
  shape id (then name) and reported as cell changed, rows/columns added or
  removed, or merge changed; rows are aligned by their cell text so an
  inserted row is one op, not a cascade of changed cells. Charts are paired
  the same way and reported as type changed, title changed, series
  added/removed, categories changed, or one values-changed op per differing
  point (old and new value; capped at 50 per series). Both families render
  in unified, JSON (`tables` / `charts` arrays) and HTML output.
- `diff` subcommand scaffold (#69): slide-level report — added, removed,
  moved, changed — from a fingerprint match (exact reorders are cheap) with a
  best-match fallback for edited slides (`--match-threshold`, default 0.5).
  Exit codes follow `git diff`: 0 identical, 1 different, 2 error. Shape,
  text and picture detail, and the unified/JSON/HTML renderers, follow in
  later Phase 4 issues.
- `diff` docs (#76): README section (matching, formats, flags, exit codes),
  `docs/git-integration.md` with two tested git recipes (`diff.pptx.textconv`
  via `pptctl extract --format txt`, and an external diff driver
  `scripts/git-pptx-diff.sh` for the full semantic diff), and a corrected
  example in `docs/diff-schema.md`.
- Picture diff (#73): pictures on a matched slide pair are paired by shape id
  (then name, groups walked) and reported as added, removed, replaced,
  re-linked (embed ↔ link) or alt-text changed. Media identity is a content
  hash, not a part name: the same image under a different `ppt/media/` name
  is not a change, and an image swap alone marks the slide changed. Each side
  carries size and, for PNG/JPEG, pixel dimensions. `Picture` gains
  `media_hash` and `dimensions`; `Package` gains a per-package hash cache
  (`part_hash`) and a header-only `read_part_prefix`. Rendering lands with #71.
- `diff` renders nested ops (#70/#72/#73 wiring): unified hunks now carry
  `-`/`+` lines for text edits, `~` lines for run reformatting and one-line
  summaries for shape and picture ops; JSON `changes` arrays are populated
  (per-op keys in `docs/diff-schema.md`); HTML lists every op. New flags
  `--strict-text` and `--geometry-tolerance`.
- `diff` output (#71): `--format unified` (default; file header, one
  `@@ slide A -> B (…) @@` hunk per slide that is not simply unchanged,
  summary line, colour on a TTY with `--color`/`--no-color` and `NO_COLOR`,
  nothing at all for identical decks), `--format json` with a versioned
  schema (`docs/diff-schema.md`; unchanged slides included, null indices for
  added/removed), `--format html` as one self-contained page, and `--quiet`
  for exit-code-only use.
- `diff` shape-level ops (#72): shapes matched by id, name, then placeholder
  type/idx (recursing into groups) and reported as added, removed, moved,
  resized, kind-changed or recolored, with EMU deltas and old/new geometry;
  sub-pixel jitter is ignored (`geometry_tolerance_emu`, default 9525 = 1 px
  at 96 DPI). The slide model gains a minimal `Shape::fill` (solid
  `a:srgbClr`/`a:schemeClr`, or `a:noFill`) so recolors are detectable.
- `diff` text ops (#70): within each changed slide, shapes are matched (id,
  name, then placeholder) and their paragraphs and the speaker notes aligned
  by LCS, reporting inserted / deleted / changed paragraphs and run
  reformatting (bold, italic, underline, size, fonts). Whitespace-only and
  field-run (`a:fld`) churn is ignored unless `strict_text` is set.
- `diff` golden fixture pairs (#75): `diff_base.pptx` plus six one-edit
  variants (text edit, reorder, slide added, slide removed, shape edit, table
  edit) from a single generator builder, with the expected unified, JSON and
  HTML output of every pair checked in under `tests/golden/`, an
  identical-pair test, and a self-diff regression net over every fixture.
- `diff` validated against 15 real-world decks with three known edits each, plus PowerPoint and Keynote round-trips (#77): `scripts/validate_diff.sh` and `docs/phase4-diff-validation.md`.

### Fixed

- `diff`: a matched slide whose only differences fall below the op tolerances (sub-pixel geometry jitter, whitespace, field runs) is now reported as unchanged instead of a `changed` slide with an empty hunk and exit code 1 (#87).
- Slide parser: a run whose text is only whitespace (`<a:t> </a:t>`, how PowerPoint stores a space between differently formatted words) was dropped, gluing words together in `extract` text and producing false text diffs after a PowerPoint re-save (#77).
- `diff`: a placeholder moved from its layout-inherited position (no `a:xfrm` on one side) was silently ignored; it is now a `moved` op carrying the explicit side's geometry and a null delta (#77).

## [0.3.0] — 2026-09-29

Phase 3: `pptctl lint`, static analysis for `.pptx` files.

### Added

- `lint` subcommand with seven rules, each with a stable id:
  `overflow` (shape past the slide boundary, group transforms applied, 0.05 in
  tolerance), `tiny-text` (below 12 pt), `alt-text` (picture without
  description), `font-risk` (font neither embedded nor bundled with the target
  platform), `empty-ph` (content placeholder with no text), `ctype` (suspect or
  undeclared content types such as printer-settings stubs and OLE objects) and
  `img-bloat` (image stored above 300 DPI effective and at least 64 KiB).
- Human-readable output sorted deck-level first then by slide, with a summary
  line; `--json` with a versioned schema (`docs/lint-schema.md`); `--strict`
  exits 2 when anything is reported (1 stays "cannot read the deck").
- Tunable thresholds: `--max-image-dpi`, `--min-image-bytes`,
  `--min-font-size`, and `--target all|windows|macos` for the font safe list.
- Header-only PNG/JPEG dimension probe in the library layer (`pptctl/image.hpp`).
- Fixtures `bloat_deck.pptx`, `lint_problems.pptx` (one slide per rule) and
  `lint_clean.pptx` (lints to zero), plus golden `lint --json` output for every
  fixture.
- `scripts/validate_lint.sh` and `docs/phase3-lint-validation.md`: the rule set
  surfaces a genuine finding in 15 of 15 real-world decks, 0 crashes under
  ASan/UBSan.

### Changed

- `Geometry` carries a group's child coordinate space (`chOff`/`chExt`).
- `human_bytes()` moved from the `info` command into `pptctl/print.hpp`.

## [0.2.0] — 2026-06-03

Phase 2: `pptctl extract`, structured content export.

### Added

- `extract` subcommand: shape tree, text frames, paragraphs and runs with
  formatting; pictures resolved through the relationship graph with media
  type and size; tables including merged cells; chart type, title, series and
  values; speaker notes.
- `--format json` (default, versioned schema in `docs/extract-schema.md`),
  `--format md`, `--format txt`.
- Golden tests with checked-in expected output per fixture and format.
- Deterministic fixture generation (`tests/fixtures/generate_fixtures.py`,
  fixed ZIP timestamps and ordering).

### Security

- Chart parsing no longer pre-allocates from the untrusted `c:ptCount`
  attribute; points are sized lazily and capped, so a malicious deck cannot
  trigger an arbitrarily large allocation.

## [0.1.0] — 2026-05-27

Phase 1: `pptctl info`, quick metadata.

### Added

- Package layer: open a `.pptx`, list parts, read part bytes, resolve
  `[Content_Types].xml` and `_rels/` relationships.
- `info` subcommand: slide count, canvas size, masters and layouts, theme fonts
  and colour scheme, media summary; `--json` output.
- Fixtures: real PowerPoint decks (`minimal.pptx`, `three_slides.pptx`) and
  synthetic decks with images, notes and a second master.
- Validated against 15 real-world decks with no crashes
  (`docs/phase1-info-validation.md`).

### Infrastructure (Phase 0)

- CMake + Ninja build with `FetchContent` dependencies (CLI11, pugixml,
  minizip-ng, nlohmann/json, doctest); presets for macOS, Linux and Windows.
- GitHub Actions matrix: Linux GCC 13 and Clang 18, macOS arm64, Windows
  MSVC 2022, plus a clang-tidy job; pre-commit hook running clang-format and
  clang-tidy.

[0.4.0]: https://github.com/valentin-best/pptctl/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/valentin-best/pptctl/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/valentin-best/pptctl/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/valentin-best/pptctl/releases/tag/v0.1.0
