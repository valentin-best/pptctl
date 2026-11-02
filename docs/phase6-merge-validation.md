# Phase 6 exit criteria — `merge` validated against real-world decks

Tracks [issue #142](https://github.com/valentin-best/pptctl/issues/142). The
Phase 6 exit criteria:
- merging ten decks from varied producers gives a deck that passes
  `pptctl lint --strict --only integrity`;
- it opens without a repair prompt, with all images intact, in PowerPoint,
  Keynote, LibreOffice and Google Slides;
- every integrity finding over the real-world corpus is triaged, and false
  positives are fixed before release.

**Verdict: met in all four renderers.**

- **Integrity:** ten decks from six producers merge into a 75-slide deck that
  passes every integrity rule, both keeping each deck's formatting and with
  `--theme=1`. So does a 428-slide merge of the whole 15-deck corpus, and a
  12-deck merge that adds two decks written by LibreOffice.
- **Same slides:** the diff engine pairs every input slide with its slot in
  the output, unchanged.
- **Renderers:** PowerPoint, Keynote, LibreOffice and Google Slides open the
  75-slide merge without a repair prompt; the 428-slide one was opened in the
  first three (not uploaded to Google Slides). Rendered to PDF and compared pixel by pixel
  with each input rendered on its own, **73 of 75 slides are identical in each
  of PowerPoint, Keynote, LibreOffice and Google Slides**. The eight
  differences are explained [below](#rendering-differences); none is a missing
  or broken image.
- **Corpus triage:** the integrity rules find **nothing** in any of the 15
  real-world decks, so there was nothing to triage and no false positive.
- **Memory safety:** 0 crashes and no AddressSanitizer/UBSan report on the
  validation merges.

Validation found **one defect**, fixed in the same PR: the merged deck kept the
base deck's `AppVersion`, so PowerPoint opened a merge onto an Office 2007 deck
in compatibility mode and redrew a newer deck's chart with different colours
([below](#defect-found-compatibility-mode-from-the-base-deck)). It also found
one fidelity gap, filed as #163: text that relies on the deck-wide default
text style picks up the base deck's ([below](#rendering-differences)). And it
found two renderer quirks that are not merge defects: Google Slides draws every
chart in the presentation's first theme, and Keynote refuses LibreOffice's
speaker notes ([below](#libreoffice-as-a-producer)).

## How to reproduce

The corpus decks are not committed; `scripts/fetch_validation_decks.sh`
downloads them, as for the earlier phases.

```bash
scripts/fetch_validation_decks.sh            # -> tests/real_world/ (gitignored)
R=tests/real_world F=tests/fixtures
scripts/validate_merge.sh $R/edu_csun_intro_jira.pptx $R/producers/keynote_export.pptx \
    $R/sample_getsamplefiles_2.pptx $R/sample_getsamplefiles_3.pptx \
    $R/sample_filesampleshub_2.pptx $R/producers/powerpoint365_resave.pptx \
    $F/merge_b.pptx $F/merge_edge.pptx $F/merge_comments_a.pptx \
    $R/edu_utsa_oral_presentation.pptx
```

The two `producers/` decks are made with the apps themselves, through
`osascript`:
- `keynote_export.pptx`: `sample_getsamplefiles_1.pptx` opened in Keynote and
  exported with `export d to … as Microsoft PowerPoint`.
- `powerpoint365_resave.pptx`: the `merge_c.pptx` fixture opened in PowerPoint
  for Mac (16.x) and saved with `save p in … as save as Open XML presentation`.

Two more, for the [LibreOffice producer check](#libreoffice-as-a-producer), are
re-saved by LibreOffice 26.8:
`soffice --headless --convert-to 'pptx:Impress MS PowerPoint 2007 XML'` of the
`merge_edge.pptx` fixture (`libreoffice_resave_edge.pptx`) and of
`edu_csun_intro_jira.pptx` (`libreoffice_resave_csun.pptx`).

`validate_merge.sh` (needs `jq`) merges the decks twice, keeping each deck's
formatting and with `--theme=1`, and for each output:
1. `pptctl merge --json`, timed: slides, parts added, dedupe, warnings.
2. `pptctl lint --strict --only integrity`, which must be clean.
3. `pptctl diff --format json` of every input against the output: every input
   slide must pair with its slot (or, for a deck given twice, either copy's).
   Keeping each deck's formatting, it must also be unchanged. Under `--theme`
   changed slides are counted, not failed: taking deck 1's layouts is the point.

Rendering: each input and the merged deck were exported to PDF by each
renderer, and `scripts/compare_slide_renders.swift MERGED.pdf SOURCE.pdf
OFFSET` compared each input's pages with its slice of the merged PDF. Slides
merged onto a larger canvas keep their geometry, so a source page is compared
with the top-left region of the merged page. The exports:
- Keynote: `export d to … as PDF`; PowerPoint: `save p in … as save as PDF`
  (both through `osascript`).
- LibreOffice 26.8: `soffice --headless --convert-to pdf`. Its PDF export
  leaves out hidden slides, so the whole-corpus merge gives 422 pages: 428
  slides less the 6 hidden ones in `edu_uva_virtualization`.
- Google Slides: each deck uploaded to Drive through the Drive v3 API with
  conversion to Google Slides, then exported as PDF through the file's
  `exportLinks`. The uploads were trashed afterwards.

## Inputs

| # | Deck | Producer | Slides | Size | Notable content |
|---|---|---|---|---|---|
| 1 | `edu_csun_intro_jira` (base) | PowerPoint 2007 (Windows) | 5 | 13.33 × 7.5 in | |
| 2 | `keynote_export` | Keynote export of a Google Slides deck | 23 | 10 × 5.62 in | photos, hyperlinks |
| 3 | `sample_getsamplefiles_2` | Google Slides export | 10 | 10 × 5.62 in | |
| 4 | `sample_getsamplefiles_3` | Google Slides export | 10 | 10 × 5.62 in | full-bleed photos (18 MB) |
| 5 | `sample_filesampleshub_2` | unknown generator (no `app.xml`) | 2 | 26.67 × 15 in | chart |
| 6 | `powerpoint365_resave` | PowerPoint for Mac 16 | 2 | 10 × 7.5 in | |
| 7 | `merge_b` | python-pptx | 2 | 10 × 7.5 in | own theme, notes, chart + workbook, sections |
| 8 | `merge_edge` | python-pptx | 3 | 10 × 7.5 in | SmartArt, linked video, hyperlink, chart |
| 9 | `merge_comments_a` | python-pptx | 2 | 10 × 7.5 in | legacy comments, threaded reply |
| 10 | `edu_utsa_oral_presentation` | PowerPoint 2010 (Windows) | 16 | 10 × 7.5 in | legacy comments, two masters |

## Results

| Merge | Slides | Output | Time | Integrity | Same slides |
|---|---|---|---|---|---|
| Keep each deck's formatting | 75 | 30.4 MiB | 1.4 s | clean | 75/75 paired, unchanged |
| `--theme=1` | 75 | 30.1 MiB | 1.3 s | clean | 75/75 paired; 38 restyled |
| Whole corpus (15 decks) | 428 | 46.4 MiB | 3.2 s, 145 MB peak RSS | clean | — |
| 12 decks, keep formatting (adds the two LibreOffice re-saves) | 83 | 30.4 MiB | 1.5 s | clean | 83/83 paired, unchanged |
| 12 decks, `--theme=1` | 83 | 30.1 MiB | 1.3 s | clean | 83/83 paired, one in its twin's slot (see [below](#libreoffice-as-a-producer)); 38 restyled |

The keep-formatting merge imports 9 masters and reuses one: `merge_comments_a`
is on the same template as `merge_edge`. The PowerPoint 365 re-save of
`merge_c` is not recognized as that template, because re-saving changed the
master's bytes. Master identity is byte equality, a documented limitation.

Under `--theme=1`, the report lists the slides whose layout had no counterpart
in deck 1 by type or name, so they fell back to Title and Content. Those are
mostly Google Slides layouts such as `MAIN_POINT` and `SECTION_TITLE_AND_DESCRIPTION`,
plus UTSA's "Title, Text, and Content".

## Rendering differences

Keeping each deck's formatting, the pixel comparison found eight differing
slides out of 300 (75 per renderer):

| Renderer | Slide | Cause | Merge at fault? |
|---|---|---|---|
| Keynote | 2 slides of `keynote_export` | Hyperlinks are drawn in the base deck's theme `hlink` colour (orange) instead of their own theme's (blue). The merged slide does reference its own theme, whose `hlink` is blue, and PowerPoint draws it blue. | No: Keynote takes hyperlink colour from the presentation's first theme. |
| PowerPoint | 1 slide of `keynote_export` | One text box wraps one line differently. It has no explicit size or spacing, so it inherits `presentation.xml`'s `p:defaultTextStyle`, which comes from the base deck (18 pt, kerned) rather than its own (14 pt). | Yes, a fidelity gap: #163. |
| PowerPoint | 1 slide of `sample_getsamplefiles_3` | A full-bleed photo differs by a few levels (mean 3.4/255); visually identical. PowerPoint re-compresses photos in its PDF export. | No: export noise. |
| LibreOffice | 2 slides of `sample_getsamplefiles_3` | A text box's lorem ipsum is larger and wraps differently. It is an orphan placeholder (`idx="4294967295"`) with no size of its own, so it falls back to `p:defaultTextStyle`, the base deck's 18 pt. | Yes: the same gap, #163. |
| Google Slides | the chart slides of `merge_b` and `merge_edge` | The bars and pie slices are drawn in the base deck's `accent1`/`accent2` (brown) and its minor font (Garamond) instead of their own theme's. The charts set no colours, so they inherit the theme's. | No: Google Slides resolves every chart against the presentation's first theme. Merging `merge_edge` then `merge_b` draws `merge_b`'s red bars in `merge_edge`'s blue, while the slide's own title keeps its theme font. |

The other three renderers resolve charts against their slide's own theme. For
Google Slides, `merge` could write the resolved colours into the charts it
imports; that would be a change to chart parts, so it is left for after v0.6.0.

## LibreOffice as a producer

Both re-saved decks pass every integrity rule on their own. In the 12-deck
merge their 8 slides render identically to the decks on their own, in
PowerPoint, Keynote and LibreOffice.

**Keynote refuses LibreOffice's speaker notes.** `libreoffice_resave_edge.pptx`
does not open in Keynote at all, before any merge (`open` returns no document).
Bisecting points to its one notes slide: LibreOffice writes `p:notes` without
`p:clrMapOvr`. The schema makes that element optional, and PowerPoint and
LibreOffice accept it. Adding
`<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>` makes the deck open, and the
merge containing it then opens too. `merge` copies the notes slide as it is, so
the merged deck inherits the problem from its input; the Keynote checks above
used the patched copy. It is the same family as the missing
`p:notesMasterIdLst` in python-pptx decks (#136), which no integrity rule
catches either.

**`--theme=1` harness note.** `merge_edge` is in the set twice in effect: once
as itself and once as LibreOffice's re-save, with the same content. Restyled
onto deck 1's layouts, the two copies of its chart slide are indistinguishable
to `pptctl diff` (similarity 1.0 each), so `validate_merge.sh` saw one paired
with its twin's slot and counted it out of place. The slide in its own slot is
the right one (`pptctl extract` output identical to the source); the script
only treats a deck given twice by path as a repeat.

## Defect found: compatibility mode from the base deck

Merge copied the base deck's `docProps/app.xml`, updating only its slide and
notes counts. With an Office 2007 base (`AppVersion` 12.0000), PowerPoint opens
the whole merged deck in compatibility mode. Content that doesn't spell out its
defaults then renders differently. `merge_b`'s chart, which has no
`c:varyColors`, showed its three bars in one colour instead of one each.

Changing only that value to 16.0000 made the chart render like its source,
while the base deck's own slides stayed pixel-identical. Merge now raises
`AppVersion` to the newest input's (`concatenate()` in
`src/lib/merge_concat.cpp`, test in `tests/test_merge_concat.cpp`).

## Corpus triage

`pptctl lint --only integrity` over the 15 real-world decks (PowerPoint
2007–2016 for Windows and Mac, three Google Slides exports, two from an
unknown generator) reports **no findings**. The two decks made for this
validation, the Keynote export, the PowerPoint 365 re-save and the two
LibreOffice re-saves, are clean too.

## Not covered

- **Producers:** OfficeCLI. Decks it writes were not part of the merge set.
- **Google Slides at scale:** the 428-slide whole-corpus merge was not
  uploaded; the 75-slide merge (30 MB) was.
