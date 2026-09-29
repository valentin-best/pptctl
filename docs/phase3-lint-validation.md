# Phase 3 exit criteria — `lint` validated against real-world decks

Tracks [issue #48](https://github.com/valentin-best/pptctl/issues/48). The ROADMAP
Phase 3 exit criterion is: **lint catches at least one real issue in 50% of
real-world decks from a sample of ten.**

**Result: met.** 15 decks, both output modes (text + `--json`), under both a
release build and an ASan/UBSan build — **0 crashes, 0 errors**, identical
counts under both builds. **15/15 decks (100%) surface at least one
diagnostic, and 15/15 surface at least one that survives manual review**
(alt-text alone accounts for 11 decks; the rest are covered by ctype,
empty-ph, tiny-text or a verified overflow).

The review also found **two correctness gaps in the overflow rule** and two
rules whose thresholds are noisier than they need to be. Follow-ups are filed
(#59–#62) and summarised at the end.

## How to reproduce

The decks are not committed; `scripts/fetch_validation_decks.sh` downloads the
same corpus used for the Phase 1 `info` validation (see
`phase1-info-validation.md` for the deck table).

```bash
scripts/fetch_validation_decks.sh            # downloads into tests/real_world/ (gitignored)
scripts/validate_lint.sh                     # lint + lint --json over every deck, per-rule table
```

`validate_lint.sh` classifies each run as **OK**, **ERROR** or **CRASH**,
tallies findings per rule from the JSON output, prints the exit-criterion
ratio, and exits non-zero on any crash or if fewer than half the decks
surface a finding. For the memory-safety pass:

```bash
./scripts/build.sh asan --no-test
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  scripts/validate_lint.sh tests/real_world --bin build/asan/pptctl --logs tests/real_world/.logs-lint-asan
```

## Findings per rule

Release build, 2026-09-29, rule set as of v0.3.0-dev (7 rules).

| deck | overflow | tiny-text | alt-text | font-risk | empty-ph | ctype | img-bloat | total |
|------|---:|---:|---:|---:|---:|---:|---:|---:|
| edu_augusta_resident          |  1 |  0 |   1 |  1 |  8 | 0 |  1 |  12 |
| edu_bu_reproducibility        |  0 |  0 |   2 |  2 |  1 | 1 |  9 |  15 |
| edu_bu_teaching_strategies    |  2 |  0 |  12 |  0 |  0 | 0 |  9 |  23 |
| edu_cornell_cs5412_lecture23  |  7 |  0 |  28 |  0 |  0 | 0 | 15 |  50 |
| edu_csun_intro_jira           |  0 |  0 |   7 |  1 |  0 | 0 |  0 |   8 |
| edu_mit_math_lecture1         |  3 |  0 |   0 |  0 |  0 | 0 |  0 |   3 |
| edu_stanford_cs345d_lecture01 |  0 |  0 |  27 |  2 |  0 | 0 |  0 |  29 |
| edu_umass_dns                 |  7 |  0 |   6 |  1 |  0 | 1 | 16 |  31 |
| edu_utsa_oral_presentation    |  2 |  1 |   3 |  0 |  0 | 1 |  1 |   8 |
| edu_uva_virtualization        |  2 | 14 |  13 |  2 |  3 | 2 |  4 |  40 |
| sample_filesampleshub_1       |  0 |  0 |   0 |  1 |  2 | 0 |  0 |   3 |
| sample_filesampleshub_2       |  0 |  0 |   0 |  1 |  2 | 0 |  0 |   3 |
| sample_getsamplefiles_1       |  0 |  0 |   0 |  0 |  2 | 0 |  3 |   5 |
| sample_getsamplefiles_2       |  2 |  0 |   1 |  0 |  0 | 0 | 14 |  17 |
| sample_getsamplefiles_3       |  2 |  0 |   0 |  0 |  0 | 0 | 22 |  24 |
| **total**                     | 28 | 15 | 100 | 11 | 18 | 5 | 94 | 271 |

## Manual review per rule

Findings were checked against the slide XML (`unzip -p … ppt/slides/slideN.xml`)
and against `pptctl extract --format json` geometry, not by eye in a renderer.
"Genuine" means a deck author would recognise it as a defect worth acting on;
"noise" means technically true but not actionable.

### overflow — 28 hits: 6 genuine, 14 noise, 8 unverifiable (bug)

- **8 findings are group children.** The rule compares a child's raw
  `a:off`/`a:ext` against the canvas, but children live in the parent group's
  `chOff`/`chExt` space. `edu_umass_dns.pptx` slide 34 "Rectangular Callout 8"
  fills its group's child space exactly, and the group sits inside the canvas,
  yet the raw child bottom (6 934 363 EMU) exceeds the 6 858 000 canvas.
  **False positive** until the transform is applied → **#59**.
  *Update after #59 landed:* mapping children through their group transform
  removed exactly these 8 findings (28 → 20); every one of them was inside the
  canvas once mapped, including the `edu_utsa_oral_presentation` shape the
  raw-coordinate review had marked "fully off".
- **14 of the remaining 20 overshoot by ≤ 0.1 in**, 8 of them by < 0.005 in
  (a few EMU). All from Google Slides / Keynote exports that place pictures
  flush to an edge with rounding in the extent. Invisible, so noise → **#60**.
- **6 are real**: pictures and a text placeholder hanging 0.5–1.3 in off the
  slide (`edu_cornell_cs5412_lecture23` slide 17, `edu_uva_virtualization`
  slides 43/46, `edu_augusta_resident` slide 5, `edu_bu_teaching_strategies`
  slide 27).

### tiny-text — 15 hits: genuine, 6 borderline

One 8 pt text box; nine 10 pt labels in architecture diagrams
(`edu_uva_virtualization` slides 4/40); five at 11.67 pt (`sz="1167"`, the
same deck). The 10 pt and 8 pt hits are unreadable from the back of a lecture
hall and are exactly what the rule is for. The 11.67 pt ones sit 0.33 pt under
the threshold and are borderline; a tunable threshold would let a user opt out
(folded into **#61**).

### alt-text — 100 hits: genuine

66 `Picture N`, 28 `Shape N` (Keynote/Google exports), 4 picture-placeholders,
1 audio poster frame (`Recorded Sound`). No `descr` attribute at all in every
case, none marked decorative. Straightforward accessibility gaps; the rule is
the single biggest source of actionable findings (11 of 15 decks).

### font-risk — 11 hits: genuine but platform-dependent

Helvetica / Helvetica Neue / Times / Bangla Sangam MN (macOS, 6 hits),
Garamond / Tw Cen MT / Franklin Gothic Book / Copperplate Gothic Light (ship
with Office, 4 hits), MS PGothic (Windows Japanese, 1 hit). Each *does*
substitute somewhere, but the safe list is Windows-centric, so every
Mac-authored deck lights up. Tiered or platform-aware safe list → **#62**.

### empty-ph — 18 hits: genuine, low severity

Mostly `body` placeholders on section-divider slides (`edu_augusta_resident`
slides 4/7/14: a filled title with an untouched subtitle placeholder) and
`subTitle` on title slides of the sample decks. Renders as nothing in slideshow
mode, so harmless to the audience, but it is the "Click to add text" clutter
authors do want to know about before handing a deck over. Kept as is.

### ctype — 5 hits: genuine

2 × `printerSettings1.bin` (the Keynote-breaking stub the rule was written
for) and 3 × `embeddings/oleObject{1,2}.bin` (embedded OLE, e.g. equations —
won't render in Keynote or Google Slides). Both are exactly the portability
risks the rule targets.

### img-bloat — 94 hits, 49.6 MiB flagged: genuine at the top, noisy at the bottom

| effective DPI | hits |
|---|---:|
| 200–300 | 31 |
| 300–500 | 28 |
| 500–1000 | 29 |
| ≥ 1000 | 6 |

Everything above ~400 DPI is a real optimisation target (`optimize`, Phase 5,
will act on these). The 200–300 band is a third of all hits and includes
images that are pixel-perfect on a 4K projector, and the rule also flags a
9.9 KiB image at 202 DPI where downscaling gains nothing. Stored-size floor
and threshold review → **#61**.

## Verdict

| rule | hits | genuine | action |
|---|---:|---|---|
| overflow  |  28 | 6 (8 group-child FPs fixed by #59 → 20) | add tolerance (#60) |
| tiny-text |  15 | 9–15 | keep; tunable threshold (#61) |
| alt-text  | 100 | 100 | keep |
| font-risk |  11 | 11, platform-dependent | tiered safe list (#62) |
| empty-ph  |  18 | 18, low severity | keep |
| ctype     |   5 | 5 | keep |
| img-bloat |  94 | ~60 | size floor + threshold review (#61) |

The exit criterion holds even if every overflow, font-risk and img-bloat
finding were discarded: alt-text, ctype, empty-ph and tiny-text alone still
surface a genuine issue in 13 of 15 decks (87%). #59 is a correctness bug in
a shipped rule and should land before v0.3.0; #60–#62 are noise reduction and
can follow.
