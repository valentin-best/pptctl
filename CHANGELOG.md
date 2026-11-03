# Changelog

All notable changes to `pptctl` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). Each version corresponds to a
ROADMAP phase and a GitHub milestone.

## [Unreleased]

### Added

- GitHub Action (#169): `uses: valentin-best/pptctl@…` lints every `.pptx`
  a pull request adds, modifies or renames, diffs it against the base, and
  writes both to the job summary, with an annotation per finding. Fails on
  lint errors by default (`fail-on`); `paths`, `lint-args`, `diff-args`,
  `html-diff` and the version are configurable. Downloads the release
  binary, or builds from source when it can't. See `docs/github-action.md`.
- pre-commit hooks (#170): `pptctl-lint` runs the `pptctl` on PATH;
  `pptctl-lint-fetch` runs the release matching the hook's `rev`, downloaded
  and checked against `SHA256SUMS` on first use. Both run `lint --strict` on
  staged `.pptx` files. See `docs/pre-commit.md`.

- `git difftool` recipe (#171): `scripts/git-pptx-difftool.sh` writes
  `pptctl diff --format html` of the two versions and opens it in the
  browser (`git difftool -t pptx -- '*.pptx'`). It ships in the release
  archives next to `git-pptx-diff.sh`; `docs/git-integration.md` gains the
  recipe and a one-block setup for all three git recipes.

- Agent skill (#172): `skills/pptctl/SKILL.md` teaches coding agents to lint
  every deck they write or edit before calling it done, and to diff, shrink
  and merge with pptctl instead of hand-editing XML. It includes the exit codes
  that aren't errors, each rule's usual fix, and a helper that fixes the two
  problems in every python-pptx deck (unlisted notes master, printer
  settings). It ships in the release archives.

- Renderer-compatibility check (#173): `scripts/renderer_compat.py` optimizes
  16 decks from 7 producers (PowerPoint for Windows and Mac, Google Slides,
  LibreOffice 6–25, ONLYOFFICE, Keynote, python-pptx) and merges them in
  pairs. It checks that headless LibreOffice converts every output with the
  same page count as its sources, and that lint finds no new integrity
  problems. The corpus is `tests/corpus/manifest.json`: openly licensed
  decks from Zenodo, pinned by SHA-256. It runs in
  `.github/workflows/renderer-compat.yml` weekly, by hand, and on pull
  requests that touch optimize or merge; the latest result (32 of 32 pass) is
  in `docs/renderer-compat.md`.

### Changed

- `git-pptx-diff.sh` hands over to `git difftool` when run inside it (#171).
  Git prefers a diff driver to the difftool, so before this `git difftool`
  on a `.pptx` printed the text diff when the driver was configured. On
  Windows it now also names the repo path in the header (the temporary
  paths it matched could hold backslashes) and recognises an added or deleted deck (`nul`).
- `pptctl lint` takes several files (#170) and exits with the worst of their
  codes. `--json` still takes one; its schema is unchanged.

## [0.6.1] — 2026-11-02

The first release with prebuilt binaries. No change to what `pptctl` does.

### Added

- Prebuilt release binaries (#168): every `v*` tag publishes archives for
  Linux x86_64 and aarch64 (static, musl), macOS arm64 (13.3+) and Windows
  x86_64 (static C run-time), with `SHA256SUMS` and the version's changelog
  section as the release notes (`.github/workflows/release.yml`). Each
  archive also carries `git-pptx-diff.sh`.

## [0.6.0] — 2026-11-02

Phase 6: `pptctl merge`, and package-integrity lint rules that serve as its
test oracle and as a verifier for decks other tools write.

Ten decks from six producers (PowerPoint 2007–16, Keynote, Google Slides,
python-pptx, an unknown generator) merge into a 75-slide deck that passes
every integrity rule and opens in PowerPoint, Keynote, LibreOffice and
Google Slides, with 73 of 75 slides pixel-identical to their sources in each
renderer. See `docs/phase6-merge-validation.md`.

### Added

- `merge` subcommand (#134): combines decks in input order into `-o FILE`
  (or nowhere, with `--dry-run`); `--json` emits a versioned report
  (`docs/merge-schema.md`). The output is built on the base deck (the
  `--theme` deck, else the first), whose parts are copied byte for byte.
- Part import (#135): each slide's closure of parts (layout, master, theme,
  media, charts with their workbooks, SmartArt, notes, comments) is copied
  under fresh names, with every relationship and content type rewritten.
- Concatenation (#136): the slide list, masters, notes master (listed in
  `p:notesMasterIdLst`, which Keynote needs) and sections; a deck without
  sections gets one named after its file when another input has some.
- Media de-duplication by content hash (#137) and collapsing of
  byte-identical slide masters (#139).
- `--theme=N` (#138): restyles every slide onto deck N's layouts, matched by
  type, then name, then Title and Content; the report names each fallback.
- Comment authors (#159): classic authors matched by name and initials,
  PowerPoint 365 authors by id.
- Integrity rules for `lint`, in `src/lib/integrity_<rule>.cpp`:
  `dangling-rel` (#129), `broken-rid` (#130), `undeclared-ctype` (#131),
  `orphan-part` (#132), `shared-part` (#133) and `unlisted-part` (#152).
  `--only` takes rule ids and the group `integrity` (#128); the integrity
  rules never parse slide content, so they run on decks whose
  `presentation.xml` is broken. Findings carry the `part` they are about
  (additive to the lint JSON schema).
- Tests: merge goldens, integrity, semantic preservation (diff and extract),
  determinism and edge inputs (#140), with new `merge_*` fixtures.
- Validation (#142): `scripts/validate_merge.sh`,
  `scripts/compare_slide_renders.swift` and
  `docs/phase6-merge-validation.md`.
- Docs: README `merge` and integrity sections, `docs/merge-schema.md` (#141).

### Fixed

- A merge kept the base deck's `AppVersion`, so PowerPoint opened a merge
  onto an Office 2007 deck in compatibility mode and redrew newer decks'
  charts differently; it now takes the newest input's (found by #142).

### Changed

- `lint`'s `ctype` rule no longer reports parts with no content type at all;
  that is now `undeclared-ctype`.
- CI: clang-tidy runs on the files a changed header reaches instead of every
  file (#157).

### Known limitations

- Text relying on the deck-wide default text style takes the base deck's
  (#163).
- Keeping each deck's formatting, Keynote draws hyperlinks and Google Slides
  draws charts (#167) in the first deck's theme colours.
- Keynote refuses decks whose notes slides lack `p:clrMapOvr`, as LibreOffice
  writes them; merge copies such notes as they are (#166).

## [0.5.0] — 2026-10-30

Phase 5: `pptctl optimize`, file-size reduction that keeps the deck the
same deck.

Over the 15-deck public validation corpus, the mean reduction is 42.9%
(median 43.4%, from 9.7% to 90.5%). That meets the Phase 5 bar on the full
corpus, but the 10 university lecture decks alone average 35.8%. What
`optimize` leaves alone is often most of what remains in those decks:
vector metafiles, OLE embeddings, and screenshots already stored at their
displayed size. See `docs/phase5-optimize-validation.md`.

### Added

- `optimize` subcommand (#98): a fixed pipeline of five passes, each with an
  opt-out, writing to `-o FILE`, `--in-place` (temp file + rename) or
  nowhere (`--dry-run`, which still measures the result). The summary
  groups each pass's actions by kind; `--json` emits a versioned report
  (`docs/optimize-schema.md`). Each pass sees the package as the previous
  one left it, and the pipeline completes every removal (the part's
  `.rels`, inbound relationships, content-type Overrides).
- `layouts` pass (#100): removes slide layouts no slide uses, masters left
  with none, and what only they reached (themes, media). A deck always
  keeps one master and a master one layout. `--no-layouts`.
- `fonts` pass (#101): removes embedded fonts and the "embed fonts" setting
  that would bring them back on the next save. `--keep-fonts`.
- `crop` pass (#104): cuts the pixels outside a picture's `a:srcRect` when
  every use of the image has the same crop, and rewrites the crop to match,
  keeping any sub-pixel remainder so the visible region is exact. PNGs are
  re-encoded losslessly; JPEGs are cropped losslessly in the DCT domain
  (new `crop_jpeg` in the codec, like `jpegtran -crop`). Images shown with
  different crops, tiled, with an SVG or effect layer, or with an EXIF
  rotation are skipped and reported. `--no-crop`.
- `images` pass (#103): downsamples PNG and JPEG images to their largest
  displayed size at the target density (`--max-scale`, default 2 x 96 DPI;
  or `--max-dpi`), by area averaging with premultiplied alpha, and
  re-encodes them: JPEG at `--quality` (default 85, progressive, ICC and
  EXIF orientation kept), PNG losslessly (alpha and colour chunks kept,
  palette when lossless). A part is replaced only when that saves at least
  1 KiB, and a same-size JPEG re-encode at least 5%. `--no-images`.
- `orphans` pass (#99): removes media nothing references. `--no-orphans`.
- Placeholder inheritance (#93): a placeholder without its own `a:xfrm` or
  fill takes it from its slide layout, else the master
  (`PlaceholderInheritance`; matched by idx, then type). `diff` compares an
  inherited position like any other, so a nudged title reports its delta
  and writing out the inherited geometry is no change; the `images` pass
  sizes placeholder pictures instead of skipping them. `extract` output is
  unchanged.
- Package writing (#96): `Package::write` copies untouched entries raw
  (still compressed) and replaces, adds or removes parts, writing to a temp
  file renamed over the target; `rewrite_rels`, `rewrite_content_types` and
  `rels_part_for` helpers.
- `PartGraph` (#97): the parts reachable from the root relationships, and a
  reverse index of who references each part.
- Image codecs (#102): libjpeg-turbo and libpng via FetchContent, with a
  strict decoder that refuses what it couldn't re-encode faithfully (CMYK
  JPEGs, 16-bit or animated PNGs).
- Tests: optimize goldens for five fixtures with same-deck (extract and
  diff), idempotence and opt-out checks (#105), and the new
  `optimize_deck.pptx` and `crop_deck.pptx` fixtures. Byte counts that
  depend on the platform's zlib are checked as invariants instead of pinned.
- Validation (#107): `scripts/validate_optimize.sh` and
  `docs/phase5-optimize-validation.md`. Over the 15 public decks: 0 crashes
  (release and ASan/UBSan), every output the same deck and stable, and
  every output opened by PowerPoint (re-saved without repair), Keynote and
  LibreOffice. No visible quality loss at 100% zoom.
- Docs: README `optimize` section and `docs/optimize-schema.md` (#106).

### Fixed

- `diff` paired a slide whose only picture changed bytes as removed + added;
  it now matches the slide and reports the picture replaced (#121).
- The `images` pass left an image untouched when downsampling it didn't
  pay off; it now falls back to a re-encode at its own size (found by #107).
- `crop_jpeg` could read one block past a source row for a grayscale JPEG
  declaring sampling factors above 1 (found by a security review, #104).

### Changed

- CI wall time cut with ccache, a working dependency cache and PR-scoped
  clang-tidy.
- Positioning: pptctl is static analysis and safe surgery for decks (lint,
  diff, optimize, merge); `fill` and library extraction are deferred
  (ROADMAP).

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

[0.6.1]: https://github.com/valentin-best/pptctl/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/valentin-best/pptctl/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/valentin-best/pptctl/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/valentin-best/pptctl/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/valentin-best/pptctl/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/valentin-best/pptctl/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/valentin-best/pptctl/releases/tag/v0.1.0
