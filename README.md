# pptctl

> Static analysis and safe surgery for `.pptx` files: lint, diff, optimize, merge — with output that opens cleanly in PowerPoint, Keynote, LibreOffice and Google Slides. Plus `info` and `extract` for quick inspection.

**Status:** early development, v0.5.0. `info`, `extract`, `lint`, `diff` and `optimize` work; `merge` and the package-integrity lint rules are on `main` for v0.6.0.

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

Editors and generators — python-pptx, PptxGenJS, agent tools like OfficeCLI — write decks. `pptctl` is what you run on them afterwards: it finds problems (`lint`), shows what changed (`diff`), shrinks them (`optimize`) and combines them (`merge`) without breaking the relationship graph. It is a single C++ binary meant for CI, git hooks and pipelines; it edits only the parts it has to and never round-trips a whole deck.

See [PLAN.md](./PLAN.md) for the long version.

## Subcommands

| Command | What it does | Status |
|---|---|---|
| `info` | Quick metadata: slide count, dimensions, fonts, media | ✅ Implemented |
| `extract` | Structured JSON/markdown export of slide content | ✅ Implemented |
| `lint` | Static analysis: overflow, tiny text, alt-text, font risk, empty placeholders, content types, image bloat, and package integrity (broken relationships, undeclared parts, orphans, shared or unlisted parts); `--json`, `--strict`, `--only <rules>` | ✅ Implemented |
| `optimize` | Shrink a deck without changing what it shows: unused layouts/masters, embedded fonts, cropped-out image regions, oversized images, orphan media; `--dry-run`, `--json` | ✅ Implemented |
| `diff` | Semantic diff: slides added/removed/moved/changed, text, shape and picture ops; `--format unified\|json\|html`, `--quiet`, exit codes like `git diff` | ✅ Implemented |
| `merge` | Combine decks in order without breaking pictures, charts, notes or comments: each keeps its own theme, or `--theme=N` restyles all onto one; identical media and masters stored once; `--dry-run`, `--json` | ✅ On `main` (v0.6.0) |
| `fill` | Template-fill from a `.pptx` template + JSON data | Deferred (see [ROADMAP.md](./ROADMAP.md)) |

## `pptctl lint`

Static analysis: finds what will look wrong on screen, or break in another app, before anyone opens the deck. Content rules check what the slides show; package-integrity rules check the file itself.

| Rule | Finds |
|---|---|
| `overflow` | A shape that runs off the slide. |
| `tiny-text` | Text below 12 pt (`--min-font-size`). |
| `alt-text` | A picture without alt text that isn't marked decorative. |
| `font-risk` | A font that is neither embedded nor installed with the target platform (`--target all\|windows\|macos`), and what it falls back to. |
| `empty-ph` | A content placeholder left empty. |
| `ctype` | A part with a content type renderers choke on (e.g. the printer-settings stub that breaks Keynote). |
| `img-bloat` | An image stored at far more resolution than it is shown at (`--max-image-dpi`, `--min-image-bytes`). |
| `dangling-rel` | A relationship to a part that doesn't exist. |
| `broken-rid` | An `r:embed` / `r:id` / `r:link` naming a relationship id the part doesn't define. |
| `undeclared-ctype` | A part with no content type in `[Content_Types].xml`. |
| `orphan-part` | A part nothing references. |
| `shared-part` | A part that must have one owner (notes, comments, charts, …) referenced from several. |
| `unlisted-part` | A slide or master the presentation relates but doesn't list: a slide that isn't shown, or a notes master Keynote refuses (python-pptx writes these for any deck with speaker notes). |

```
$ pptctl lint --only integrity broken.pptx
broken.pptx:ppt/slides/_rels/slide1.xml.rels: error[dangling-rel]: relationship rId4 (image) targets "ppt/media/missing.png", which does not exist
broken.pptx:ppt/notesSlides/notesSlide1.xml: error[shared-part]: notesSlide part "ppt/notesSlides/notesSlide1.xml" is shared by ppt/slides/slide1.xml, ppt/slides/slide2.xml; each needs its own copy
broken.pptx:ppt/presentation.xml: error[unlisted-part]: relationship rId11 (slide) targets "ppt/slides/slide9.xml", but ppt/presentation.xml doesn't list it in p:sldIdLst: it is not part of the slide show
broken.pptx:ppt/slides/slide1.xml: error[broken-rid]: r:embed="rId99" is not defined in ppt/slides/_rels/slide1.xml.rels
…
broken.pptx: 2 warnings, 6 errors
```

`--only` takes rule ids and the group `integrity` (comma-separated or repeated). `--only integrity` is how to **verify a deck another tool wrote** (python-pptx, PptxGenJS, agent tools like OfficeCLI) before it reaches PowerPoint's repair prompt or Keynote's refusal; it never parses slide content, so it also runs on decks whose `presentation.xml` is broken. `--strict` exits `2` when anything is found, for CI:

```bash
pptctl lint --strict --only integrity generated.pptx
```

`--json` emits a versioned report; see [docs/lint-schema.md](./docs/lint-schema.md). Exit codes: `0` read (findings or not), `1` unreadable, `2` findings under `--strict`.

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

Validated on a corpus of real-world decks: see [docs/phase4-diff-validation.md](./docs/phase4-diff-validation.md).

## `pptctl optimize`

Shrinks a deck without changing what it shows. Five passes run in this order, each with an opt-out:

| Pass | What it removes | Opt-out |
|---|---|---|
| `layouts` | Slide layouts no slide uses, masters left with none, and what only they used (themes, media) | `--no-layouts` |
| `fonts` | Embedded fonts, and the "embed fonts" setting that would put them back on the next save | `--keep-fonts` |
| `crop` | The pixels outside a picture's crop; PowerPoint keeps the whole photo behind a 10 % sliver | `--no-crop` |
| `images` | Pixels beyond what each image needs at its largest displayed size (2 × 96 DPI by default: Retina at 100 % zoom), then a re-encode | `--no-images` |
| `orphans` | Media nothing references | `--no-orphans` |

```
$ pptctl optimize -o lean.pptx deck.pptx
deck.pptx -> lean.pptx
  layouts: 9 removed (layout x9), 35.2 KiB -> 0 B
  fonts: nothing to do
  crop: 11 cropped (1280x1024 -> 305x321, 480x640 -> 400x530, 1280x1024 -> 658x822, 1024x768 -> 304x234, +7 more), 4.4 MiB -> 2.5 MiB
  images: 1 recompressed (670x460), 2 downsampled (1280x522 -> 866x353, 681x681 -> 465x465), 1.7 MiB -> 941.8 KiB
  orphans: nothing to do
total: 4.8 MiB -> 2.1 MiB (-55%)
```

| Flag | Meaning |
|---|---|
| `-o FILE` / `--in-place` / `--dry-run` | Where the result goes: a new file, over the input (written to a temp file and renamed), or nowhere (the report still shows the resulting size). One is required. |
| `--json` | The report as versioned JSON instead; see [docs/optimize-schema.md](./docs/optimize-schema.md). |
| `--quality N` | JPEG re-encode quality, 1-100 (default 85). |
| `--max-scale F` | Pixels kept per 96 DPI canvas pixel at an image's largest displayed size (default 2). |
| `--max-dpi N` | Pixels kept per displayed inch instead (overrides `--max-scale`). |

What it promises:

- **Surgical.** Only the parts a pass changes are rewritten; every other ZIP entry is copied byte for byte, still compressed. Relationships and `[Content_Types].xml` are updated for every part removed.
- **Never worse.** A part is replaced only if the result is smaller (by at least 1 KiB). PNGs are re-encoded losslessly, and JPEG crops are lossless (DCT-domain, like `jpegtran -crop`). A JPEG is re-encoded at the same size only if that saves at least 5 %. Vector (EMF/WMF/SVG), GIF, TIFF, video and audio media are never re-encoded, only removed when nothing uses them.
- **Same deck.** On the validation corpus, `pptctl diff` between input and output shows only replaced pictures, `pptctl extract` is identical apart from picture sizes, and a second `optimize` finds nothing to do. Every output opens in PowerPoint (and re-saves without a repair prompt), Keynote and LibreOffice.

How much it saves depends on what a deck is made of. Over 15 public decks the mean reduction is **42.9 %** (median 43.4 %, from 9.7 % to 90.5 %), but over the 10 university lecture decks among them it is **35.8 %**. Photo-heavy decks with oversized or cropped images shrink the most. What `optimize` leaves alone is often most of what remains in the others: vector metafiles, embedded OLE objects, and screenshots already stored at the size they are shown. `--max-scale 1` (96 DPI) saves more, about 50 % on the same corpus, at the cost of sharpness on high-DPI screens. Full numbers, renderer checks and method: [docs/phase5-optimize-validation.md](./docs/phase5-optimize-validation.md).

Exit codes: `0` when the deck was optimized or measured (even if nothing changed), `1` when the input can't be read, no output was given, or the output can't be written.

## `pptctl merge`

Combines decks into one, slides in input order. Every picture, chart (with its embedded workbook), SmartArt diagram, speaker note, comment and link arrives working: each part a slide uses is copied under a fresh name and every relationship pointing at it is rewritten, which is exactly what naive merges get wrong.

```
$ pptctl merge intro.pptx results.pptx outro.pptx -o talk.pptx
merge -> talk.pptx
  1. intro.pptx: 3 slides (base)
  2. results.pptx: 2 slides, 21 parts added, 1 master imported
  3. outro.pptx: 2 slides, 2 parts added, 1 media deduplicated (143 B), 1 master reused
total: 7 slides, 65.7 KiB
```

**Formatting.** By default each deck keeps its own look: its slides bring their slide master, layouts and theme. Decks made from the same template don't each bring a copy; an identical master is stored once (`master reused` above). `--theme=N` instead puts every slide on deck N's layouts and drops the other decks' masters and themes, so the result reads as one deck. Each slide goes to the layout of the same type (title, title and content, title only, …), else of the same name, else Title and Content; the report names any slide that needed that last fallback. A slide's own formatting stays; what it inherited from its old layout now comes from deck N's, so expect fonts, colours and placeholder positions to change: that is the point of the option.

**Storage.** A picture, video or audio file identical to one already in the output is stored once (`media deduplicated`). Single-owner parts (notes, charts, workbooks, diagrams, comments) are always copied, one per slide.

**The base deck.** The output is built on the `--theme` deck, or the first: its parts are copied byte for byte, and only `[Content_Types].xml`, `presentation.xml`, its relationships and `docProps/app.xml` are rewritten. Presentation-wide settings come from it too.

| Flag | Meaning |
|---|---|
| `-o FILE` / `--dry-run` | Where the result goes; it can't be one of the inputs. `--dry-run` writes nothing but still measures the result. One is required. |
| `--theme N` | Restyle every slide onto deck N's layouts (1-based). Default: each deck keeps its own. |
| `--json` | The report as versioned JSON; see [docs/merge-schema.md](./docs/merge-schema.md). |

Also carried over: sections (a deck without any gets one named after its file, when some input has sections), the notes master (imported notes use the base's; the first imported one is used if the base has none), and comment authors (matched by name and initials for classic comments, by id for PowerPoint 365 comments). The same inputs always produce the same bytes, and merging well-formed decks gives a deck that passes `pptctl lint --only integrity` (a defect already in an input, such as a dangling relationship, is copied as is and reported).

Limitations, by design for now:

- **Masters are shared only when byte-identical.** Two copies of a template that differ by a re-save, whitespace or one changed colour are imported separately; `pptctl optimize` then removes the layouts no slide uses.
- **No resizing.** Slides from a deck with another slide size keep their geometry on the base's canvas, with a warning in the report.
- **Presentation-wide settings come from the base deck.** Another deck's embedded fonts, custom shows and handout master are not carried over; `pptctl lint` reports text whose font then needs a substitute (`font-risk`).

Exit codes: `0` merged (or measured), `1` when an input can't be read, `--theme` is out of range, `-o` names an input, or the output can't be written. Command-line errors (a missing file, fewer than two inputs) are rejected by the argument parser with a non-zero code.

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
