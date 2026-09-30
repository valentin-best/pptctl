# Phase 4 exit criteria — `diff` validated against real-world decks

Tracks [issue #77](https://github.com/valentin-best/pptctl/issues/77). The ROADMAP
Phase 4 exit criterion is: **diffing two real-world decks where you made known
small edits produces a diff that matches what you changed.**

**Result: met.** 15 decks, three known edits each (a text edit, a shape move,
a slide removed), under both a release build and an ASan/UBSan build:
**0 crashes, 15/15 self-diffs identical, 15/15 python-pptx round-trips
identical, 15/15 decks where every known edit is reported, 0 spurious slides,
0 extra ops.** The same edits saved through PowerPoint for Mac still score
3/3 with nothing spurious, and a pure PowerPoint re-save of the original is
identical. A Keynote export is not (see below).

The validation found **three engine defects**, all fixed in the same PR
(#92): a dropped whitespace-only run, a silently ignored placeholder move, and
the empty-hunk case from #87. One follow-up is filed (#93).

## How to reproduce

The decks are not committed; `scripts/fetch_validation_decks.sh` downloads the
same corpus as the Phase 1 and Phase 3 validations.

```bash
scripts/fetch_validation_decks.sh            # -> tests/real_world/ (gitignored)
scripts/validate_diff.sh                     # self-diff, re-save, known edits, timing
```

`validate_diff.sh` drives `scripts/validate_diff_edits.py` (python-pptx via
`uv`, dev-only) which writes, per deck, a `.resaved.pptx` (no edits, the
round-trip noise floor), an `.edited.pptx` with three known edits, and an
`.expected.json` describing them. The script then runs `pptctl diff` in JSON
and scores each expectation, counts slides that are reported outside the
edited ones (spurious) and ops on the edited slides beyond the one made
(extra), and times the largest pair. For the memory-safety pass:

```bash
./scripts/build.sh asan --no-test
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  scripts/validate_diff.sh tests/real_world --bin build/asan/pptctl --work tests/real_world/.work-diff-asan
```

## Results

| deck | slides | self | re-saved | known edits | spurious |
|---|---|---|---|---|---|
| edu_augusta_resident | 27 | identical | identical | 3/3 | 0 |
| edu_bu_reproducibility | 44 | identical | identical | 3/3 | 0 |
| edu_bu_teaching_strategies | 53 | identical | identical | 3/3 | 0 |
| edu_cornell_cs5412_lecture23 | 45 | identical | identical | 3/3 | 0 |
| edu_csun_intro_jira | 24 | identical | identical | 3/3 | 0 |
| edu_mit_math_lecture1 | 24 | identical | identical | 3/3 | 0 |
| edu_stanford_cs345d_lecture01 | 41 | identical | identical | 3/3 | 0 |
| edu_umass_dns | 54 | identical | identical | 3/3 | 0 |
| edu_utsa_oral_presentation | 20 | identical | identical | 3/3 | 0 |
| edu_uva_virtualization | 42 | identical | identical | 3/3 | 0 |
| sample_filesampleshub_1 | 1 | identical | identical | 1/1 | 0 |
| sample_filesampleshub_2 | 2 | identical | identical | 2/2 | 0 |
| sample_getsamplefiles_1 | 6 | identical | identical | 3/3 | 0 |
| sample_getsamplefiles_2 | 7 | identical | identical | 3/3 | 0 |
| sample_getsamplefiles_3 | 8 | identical | identical | 3/3 | 0 |

The two one- and two-slide decks get fewer edits because the script only
removes a slide that carries none of the other edits.

**Timing.** Largest pair, `sample_getsamplefiles_3` (18 MB, 8 slides, mostly
media): 0.40 s wall, 18 MB peak RSS. The 54-slide `edu_umass_dns` pair: under
0.2 s. Memory tracks the largest single slide plus one media hash pass, not
the deck, consistent with the one-slide-at-a-time summary design.

## Application round-trips

Done on `edu_umass_dns.pptx` (54 slides) by scripting the installed apps
(`osascript`); not part of `validate_diff.sh` because it needs the GUI apps.

**PowerPoint for Mac.** Open, save as Open XML presentation, close:

- original vs re-save: **identical** (exit 0). PowerPoint merges adjacent
  runs with identical formatting and drops `smtClean`; both are invisible to
  the diff by design (run re-splitting is not a change; the fingerprint
  difference is downgraded because no op family reports anything, #87).
- original vs the python-pptx-edited deck re-saved by PowerPoint: exactly the
  three known edits, nothing else.

Before the fixes below this pair showed 16 empty hunks and two text edits
that were not edits.

**Keynote.** Open, export as PowerPoint, close: **51 of 54 slides changed**,
about 1,100 ops. This is not tolerance noise: Keynote flattens everything it
inherited from the layout, master and theme into explicit slide XML, and
re-lays out some content.

| what Keynote's export does | how it shows | count |
|---|---|---|
| writes explicit solid fills where the slide inherited "no fill" | `recolored from no fill to ...` / `... to inherited` | ~250 |
| writes explicit positions for inherited placeholders, often different ones | `moved from its inherited layout position to (...)` | ~50 |
| moves and resizes shapes it re-lays out (most deltas over 0.5 in) | `moved by` / `resized by` | ~580 |
| writes explicit run sizes / bold / italic that were inherited | `~shape ...: size 24pt -> inherit` | ~200 |
| rewrites picture descriptions | `alt text changed` | ~90 |
| re-encodes connectors as text boxes and text boxes as groups | `changed kind from ...` | ~36 |

Every one of these is a real difference in the slide XML, so `diff` is
correct to report them, but they are not what a user changed. Collapsing the
first, second and fourth rows needs inheritance resolution (layout, master,
theme), filed as #93. The rest is genuinely Keynote re-laying out the deck.

## Defects found and fixed

1. **Whitespace-only runs were dropped by the parser** (`Slide::parse`).
   pugixml skips whitespace-only text nodes unless `parse_ws_pcdata_single`
   is set, so `<a:r><a:t> </a:t></a:r>` (the way PowerPoint stores a space
   between two differently formatted words) parsed as an empty run and
   `www.google.com` + ` ` + `= 74…` came out as `www.google.com= 74…`. When
   PowerPoint merged that run into its neighbour on re-save, the diff
   reported a text change. Fixed in the slide parser; `extract` text output
   gains the missing spaces too. No committed fixture had such a run, so no
   golden changed.
2. **A placeholder moved from its inherited position was silently ignored.**
   `diff_shapes` compared geometry only when both sides had an `a:xfrm`.
   Moving an inherited placeholder in PowerPoint writes one, so 7 of the 15
   decks lost their "move" edit. Now reported as `moved` with the explicit
   side's geometry and a null delta, described as "moved from its inherited
   layout position to (x, y)" (and the reverse for a reset to layout).
3. **Empty hunks** (#87, merged with this): a changed fingerprint with no op
   to show, from run re-splitting or sub-tolerance jitter, made the slide
   `changed` and the exit code 1.

## Follow-ups

- **#93 — resolve inherited placeholder geometry and fills from the layout
  and master.** Gives a real delta for the most common real-world edit
  (nudging a title), removes the null-delta special case, and collapses the
  bulk of cross-application noise (Keynote, and likely LibreOffice and Google
  Slides exports). Also benefits lint's overflow rule, which has the same
  blind spot.
- The LIS tie-break for reorders (#87, cosmetic) is unchanged.
