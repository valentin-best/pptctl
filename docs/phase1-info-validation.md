# Phase 1 exit criteria — `info` validated against real-world decks

Tracks [issue #6](https://github.com/valentin-best/pptctl/issues/6). The ROADMAP
Phase 1 exit criterion is: **`pptctl info` runs on ten varied real-world `.pptx`
files from the wild without crashing.**

**Result: met.** 15 decks, both output modes (text + `--json`), under both a
release build and an ASan/UBSan build — **0 crashes, 0 errors**, all JSON output
parses cleanly. No bugs found.

## How to reproduce

The decks are **not committed** (size + unclear licensing on wild decks). The
corpus is instead captured as a URL manifest in `scripts/fetch_validation_decks.sh`,
so a run is two commands:

```bash
scripts/fetch_validation_decks.sh            # downloads into tests/real_world/ (gitignored)
scripts/validate_info.sh                     # runs `info` over every deck, classifies each
```

`validate_info.sh` runs `info` and `info --json` on each deck and classifies the
outcome as **OK** (exit 0), **ERROR** (clean non-zero exit), or **CRASH** (killed
by a signal — segfault, abort, ASan trap). It exits non-zero if any deck crashes.

To also check memory safety, point it at the ASan build:

```bash
./scripts/build.sh asan --no-test
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  scripts/validate_info.sh tests/real_world --bin build/asan/pptctl --logs tests/real_world/.logs-asan
```

> Note: on macOS / Apple Clang, do **not** set `ASAN_OPTIONS=detect_leaks=1` —
> LeakSanitizer is unsupported there and ASan aborts before running the program.

Expect some link-rot in the manifest over time; the criterion only needs ~10
decks to download and pass.

## Corpus

Public lecture decks from university (`.edu`) sites plus a few sample-file hosts,
chosen for variety in slide count, canvas size, theme, fonts, and media weight.
Authored across many years (2012–2019) by different tools.

| deck | slides | canvas (in) | images | parts |
|------|-------:|-------------|-------:|------:|
| edu_augusta_resident          | 44 | 14.22 × 10.67 |  4 | 120 |
| edu_bu_reproducibility        | 63 | 10.00 × 7.50  | 19 | 187 |
| edu_bu_teaching_strategies    | 37 | 10.00 × 7.50  | 13 | 173 |
| edu_cornell_cs5412_lecture23  | 41 | 10.00 × 5.62  | 18 | 224 |
| edu_csun_intro_jira           |  5 | 13.33 × 7.50  | 13 |  71 |
| edu_mit_math_lecture1         | 41 | 10.00 × 7.50  | 40 | 164 |
| edu_stanford_cs345d_lecture01 | 30 | 10.00 × 7.50  | 24 | 125 |
| edu_umass_dns                 | 54 | 10.00 × 7.50  | 20 | 175 |
| edu_utsa_oral_presentation    | 16 | 10.00 × 7.50  | 11 | 150 |
| edu_uva_virtualization        | 51 | 10.00 × 6.25  | 21 | 182 |
| sample_filesampleshub_1       |  1 | 26.67 × 15.00 |  0 |  48 |
| sample_filesampleshub_2       |  2 | 26.67 × 15.00 |  0 |  53 |
| sample_getsamplefiles_1       | 23 | 10.00 × 5.62  | 11 | 146 |
| sample_getsamplefiles_2       | 10 | 10.00 × 5.62  | 14 |  94 |
| sample_getsamplefiles_3       | 10 | 10.00 × 5.62  | 19 | 103 |

Coverage spans 4:3, 16:9, 16:10 and oversized canvases; Office, Focus, Organic
and custom theme color schemes; Calibri/Arial/Garamond fonts; decks with no media
through to ~18 MB of embedded images; and packages from 48 to 224 parts.

## Run record

- Date: 2026-05-27
- Platform: macOS (arm64), Apple Clang 21
- Binary version: `pptctl 0.0.1-dev`
- Builds: `macos-release` and `asan` (AddressSanitizer + UBSan)
- Decks: 15 · text + `--json` each · **15 OK / 0 errors / 0 crashes** in both builds
