# Changelog

All notable changes to `pptctl` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each version corresponds to a
ROADMAP phase and a GitHub milestone.

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

[0.3.0]: https://github.com/valentin-best/pptctl/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/valentin-best/pptctl/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/valentin-best/pptctl/releases/tag/v0.1.0
