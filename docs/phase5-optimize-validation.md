# Phase 5 exit criteria — `optimize` validated against real-world decks

Tracks [issue #107](https://github.com/valentin-best/pptctl/issues/107). The
Phase 5 exit criterion is: **average ≥40% reduction over 10 public decks, no
visible quality loss in PowerPoint + Keynote.**

**Result.** Over the full 15-deck public corpus, the mean reduction is
**42.3%** (median 37.0%, from 6.7% to 90.5%). Over the 10 university lecture
decks alone it is **34.9%**. Whether the bar is met depends on which ten decks
count; see [Reduction](#reduction).

Every output is the same deck:
- **0 crashes**, under both a release build and an ASan/UBSan build.
- **15/15 diffs** show only replaced pictures, with every slide paired.
- **15/15 extracts** are identical once picture sizes are set aside.
- **15/15 outputs are stable:** optimizing again changes nothing.
- **15/15 outputs open in all three apps.** PowerPoint for Mac re-saves each one without a repair prompt, Keynote exports each one, and LibreOffice converts each one, all with the original slide count.
- **No visible quality loss** at 100% zoom on the three most image-heavy decks.

Validation found **one defect**, fixed in the same PR. The images pass
dropped an image entirely when downsampling it didn't pay off, instead of
falling back to a lossless recompress at its own size
([below](#defect-found-downsampling-that-doesnt-pay-skipped-the-image)).

## How to reproduce

The decks are not committed; `scripts/fetch_validation_decks.sh` downloads the
same corpus as the Phase 1, 3 and 4 validations.

```bash
scripts/fetch_validation_decks.sh            # -> tests/real_world/ (gitignored)
scripts/validate_optimize.sh                 # sizes, pass breakdown, timing, same-deck, stability
```

`validate_optimize.sh` (needs `jq`) runs each deck through the steps below and prints a table and a summary. It exits non-zero on a crash or any failed check.
1. `pptctl optimize --json -o` with the defaults: 2× density (192 DPI) and JPEG quality 85.
2. `pptctl diff --format json` of input against output. It must pair every slide, with no slide moved, and the only allowed op is a picture `replaced`.
3. `pptctl extract --format json` of both. They must be identical once the input path and picture `size_bytes` are dropped.
4. A second `optimize --dry-run` of the output, which must find nothing to do.

For the memory-safety pass:

```bash
./scripts/build.sh asan --no-test
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  scripts/validate_optimize.sh tests/real_world --bin build/asan/pptctl --work tests/real_world/.work-optimize-asan
```

The application checks (below) script the installed apps with `osascript`. They aren't part of the script because they need the GUI apps.

## Reduction

| deck | before | after | saved | time | peak RSS | saved by pass |
|---|---:|---:|---:|---:|---:|---|
| edu_augusta_resident | 676 KiB | 451 KiB | 33.2% | 0.38 s | 16 MB | layouts 58K, crop 146K, images 24K |
| edu_bu_reproducibility | 1.8 MiB | 1.2 MiB | 34.0% | 0.57 s | 39 MB | layouts 52K, crop 2K, images 612K |
| edu_bu_teaching_strategies | 4.8 MiB | 2.1 MiB | 55.1% | 3.16 s | 28 MB | layouts 35K, crop 1.9M, images 753K |
| edu_cornell_cs5412_lecture23 | 12.5 MiB | 1.2 MiB | 90.5% | 2.72 s | 158 MB | layouts 42K, fonts 787K, images 10.5M |
| edu_csun_intro_jira | 1.3 MiB | 963 KiB | 29.3% | 1.39 s | 31 MB | layouts 65K, crop 3K, images 354K |
| edu_mit_math_lecture1 | 1.8 MiB | 1.2 MiB | 37.0% | 2.30 s | 38 MB | layouts 35K, images 645K |
| edu_stanford_cs345d_lecture01 | 1.2 MiB | 1.0 MiB | 15.7% | 0.84 s | 8 MB | layouts 34K, crop 58K, images 121K |
| edu_umass_dns | 1.5 MiB | 1.4 MiB | 6.7% | 0.99 s | 22 MB | layouts 36K, crop 29K, images 48K |
| edu_utsa_oral_presentation | 1.3 MiB | 995 KiB | 25.8% | 0.37 s | 18 MB | layouts 84K, crop 95K, images 212K |
| edu_uva_virtualization | 1.6 MiB | 1.2 MiB | 21.4% | 2.00 s | 129 MB | layouts 23K, images 270K |
| sample_filesampleshub_1 | 33 KiB | 14 KiB | 57.9% | 0.01 s | 4 MB | layouts 42K |
| sample_filesampleshub_2 | 41 KiB | 22 KiB | 47.3% | 0.01 s | 4 MB | layouts 42K |
| sample_getsamplefiles_1 | 1.0 MiB | 599 KiB | 43.4% | 1.32 s | 113 MB | layouts 134K, fonts 414K, images 24K |
| sample_getsamplefiles_2 | 9.7 MiB | 4.7 MiB | 51.9% | 1.32 s | 113 MB | layouts 20K, fonts 241K, crop 1.9M, images 3.0M |
| sample_getsamplefiles_3 | 18.0 MiB | 2.6 MiB | 85.4% | 5.27 s | 177 MB | layouts 59K, fonts 453K, crop 5.7M, images 9.3M |

Pass sizes are uncompressed part bytes saved, so they don't add up to the
file-size change exactly. The orphans pass found nothing to remove in this
corpus (it is exercised by the `optimize_deck` golden). Timing and RSS are for the whole run (all five passes, the
intermediate writes and the final write) on an M1 Pro; the largest deck
(18 MB) takes 5.3 s and 177 MB.

**Against the 40% bar.** The mean over all 15 public decks is 42.3%. The 10
lecture decks alone average 34.9%. Their remaining weight is mostly content
the passes deliberately leave alone:

- **Vector metafiles.** `edu_umass_dns` keeps a 2 MB EMF (558 KB stored), and `edu_uva_virtualization` keeps two EMFs of 0.5 and 0.8 MB. Rasterizing a metafile would trade a vector for pixels.
- **OLE embeddings.** `edu_utsa_oral_presentation` keeps a 1.1 MB `oleObject1.bin`.
- **Screenshots already at the size they're shown,** which only a lossless recompress can touch: `edu_stanford_cs345d_lecture01`.
- **Placeholder pictures that inherit their geometry** from the layout: 4 in `edu_umass_dns` and 9 in `edu_mit_math_lecture1`. The images pass skipped them; #93 resolved them afterwards (see [After #93](#after-93)).

The defaults could reach the bar on the lecture decks only by giving up
Retina headroom:

| settings | mean, 15 decks | mean, 10 lecture decks |
|---|---:|---:|
| defaults (`--max-scale 2`, `--quality 85`) | 42.3% | 34.9% |
| `--max-scale 1.5` | 44.3% | 36.9% |
| `--quality 75` | 44.0% | 37.0% |
| `--max-scale 1` (96 DPI) | 50.1% | 43.8% |

The defaults are unchanged. 192 DPI is what a Retina display shows at 100%
zoom, and lowering it is the trade the flags exist for.

## Application checks

On every one of the 15 outputs:

- **PowerPoint for Mac.** Open, count slides, save as Open XML presentation, close. All 15 open without a repair prompt (a repair prompt is modal, so it would have stalled the scripted run), re-save, and keep their slide count.
  - Recipe note: PowerPoint is sandboxed. Saving into a folder it hasn't been granted raises a *Grant File Access* sheet that blocks the script. Work under `build/manual_check/`.
- **Keynote.** Open, count slides, export to PDF, close. All 15 open and export, with the original slide count.
- **LibreOffice** 7.4 (Debian bookworm, headless in an OrbStack container, so nothing is installed on the host). Converts every original and every output to PDF, and the page counts are equal for each pair. LibreOffice drops hidden slides from the PDF, so `edu_uva_virtualization` has 45 pages on both sides.

## Visual check

On the three most image-heavy decks (`sample_getsamplefiles_3`,
`edu_cornell_cs5412_lecture23`, `sample_getsamplefiles_2`), every picture
optimize changed was rendered both ways at its displayed size at 192 DPI,
which is 100% zoom on a Retina display. Each version was rendered from its own
image and `srcRect` into the frame with an exact box resample, then compared
side by side and by PSNR.

- **No artefacts.** There's no blocking, ringing or colour shift, and no misplaced crop. A shift search finds every picture aligned at offset 0, with no sub-pixel offset. The optimized side is very slightly softer. That comes from the second resample: the image is sized up to the next whole pixel at 192 DPI, then drawn into the frame.
- **JPEG.** Against a reference built from the original by an exact box resize to the same size, and a baseline of that reference re-encoded at quality 85 by Pillow (also libjpeg-turbo): our output measures 36.1 dB (median), the baseline 37.9 dB. That's the expected cost of quality 85 plus slightly different resampling.
- **PNG.** Re-encoded losslessly, so the only difference from the reference is resampling: 39.2 dB median on `edu_cornell_cs5412_lecture23` and 43.4 dB on `sample_getsamplefiles_2`. The lowest, 30 dB, is a white-on-black text screenshot. The difference sits on glyph edges, it isn't visible at 100%, and there's no offset.
- **Crop.** The lossless JPEG crop of a 2500x1667 photo matches the original at the kept offset (x = 496) with zero difference.

No tuning of the defaults was needed.

## Defect found: downsampling that doesn't pay skipped the image

Running with `--max-scale 1.5` saved *less* than the default on
`edu_mit_math_lecture1` (1% vs 37%), though a lower density should only ever
save more.
- **What happened.** Its 31 screenshots are palette PNGs. At 2× they need all their pixels, so they get a lossless recompress, which saves 44%. At 1.5× they need fewer pixels, so the pass downsampled them. Resampling blends their colours into a truecolour PNG that is larger than the original, so the "never grow" rule rejected it, and the image was left as it was.
- **Fix** (`src/lib/optimize_images.cpp`). When a downsampled image doesn't pass, the image is re-encoded at its own size under the same rules: lossless for PNG, at least 5% saved for a JPEG.
- **Test.** `optimize images: a PNG that downsampling would grow is still recompressed`: a 16-colour noisy palette PNG shown at 75% of its pixels.
- **Effect at the defaults.** The mean rose from 41.5% to 42.3%: `edu_uva_virtualization` went from 9.9% to 21.4%, and `edu_cornell_cs5412_lecture23` saved a further 62 KB. The settings sweep is now monotonic.

## Follow-ups

- **#93** (inherited placeholder geometry) has since landed; see below.
- Nothing new is filed. The remaining weight in the low-scoring decks is vector metafiles, OLE embeddings and screenshots already at their displayed size, which optimize leaves alone by design.

## After #93

[#93](https://github.com/valentin-best/pptctl/issues/93) resolves a placeholder's geometry from its layout or master. The images pass now sizes the 13 placeholder pictures it used to skip, and no deck reports an inherited-geometry skip.

- **Results:** `edu_mit_math_lecture1` goes from 37.0% to 43.5%, and `edu_umass_dns` from 6.7% to 9.7%. The mean is **42.9%** over the 15 decks (median 43.4%), and **35.8%** over the 10 lecture decks.
- **Quality:** all 13 pictures already had at most 192 DPI in their inherited frames (umass's run from 107 to 168 DPI). So they are recompressed or cropped losslessly, never downsampled, with no quality change.
- **Re-checks:** the two changed outputs pass the same-deck and stability checks, PowerPoint re-save, Keynote PDF export and LibreOffice conversion, all with their original slide counts. The other 13 outputs are byte-identical.
