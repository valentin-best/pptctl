# Phase 7 exit criteria: integrations validated on a sample repo

Tracks [issue #174](https://github.com/valentin-best/pptctl/issues/174). The
Phase 7 exit criterion:

> a sample repo where a PR touching a `.pptx` gets lint findings and a
> readable diff with no manual setup beyond adding the Action.

**Verdict: met.** In a fresh private repository whose only setup is the
seven-line workflow from the README, every pull request that touched a deck
got a job summary with a slide diff and its lint findings, plus an annotation
per finding on the deck. The check failed exactly when `fail-on` said it
should. A pull request with no decks finished in 19 seconds. Both pre-commit
hooks blocked bad decks in the same repository.

Validation found **one documentation fix**, made in this PR: the workflow
snippet used `actions/checkout@v4`, which now triggers GitHub's "Node.js 20 is
deprecated" warning on every run. The docs now say `@v7`, re-tested below. It
also confirmed a known cost of the repository being private: consumers build
pptctl from source, about 4 minutes per run.

## Setup

- Sample repo: `valentin-best/pptctl-sample-decks` (private, same owner).
  `main` holds two decks and `.github/workflows/decks.yml`, copied from the
  README:

  ```yaml
  name: decks
  on: pull_request
  jobs:
    review:
      runs-on: ubuntu-latest
      steps:
        - uses: actions/checkout@v4   # @v7 in the docs now, see below
        - uses: valentin-best/pptctl@main
  ```

- One setting on pptctl, needed only while it is private: *Settings ›
  Actions › General › Access › Accessible from repositories owned by the user*.
  Without it a workflow in another repository can't `uses:` the Action.
- The Action at `main` (`8397666`); no release includes it yet, so `@main` it is.

## Pull requests

| PR | Change | Expected | Result | Run |
|---|---|---|---|---|
| [#1](https://github.com/valentin-best/pptctl-sample-decks/pull/1) | `decks/roadmap.pptx` modified: text edit, slide moved, pictures replaced, removed and added, alt text changed | pass, diff shown | **pass**; diff and 1 warning | 299 s |
| [#2](https://github.com/valentin-best/pptctl-sample-decks/pull/2) | clean deck added | pass | **pass**; "clean" | 236 s |
| [#3](https://github.com/valentin-best/pptctl-sample-decks/pull/3) | deck deleted | pass | **pass**; listed as deleted | 278 s |
| [#4](https://github.com/valentin-best/pptctl-sample-decks/pull/4) | deck with 6 integrity errors added | fail (`fail-on: error`, the default) | **fail**; 6 errors, 3 warnings | 246 s |
| [#5](https://github.com/valentin-best/pptctl-sample-decks/pull/5) | README only | pass, no download | **pass**, nothing installed | **19 s** |
| [#6](https://github.com/valentin-best/pptctl-sample-decks/pull/6) | `fail-on: warning`, deck with 7 warnings added | fail | **fail** | 200 s |
| [#7](https://github.com/valentin-best/pptctl-sample-decks/pull/7) | `fail-on: never`, deck with integrity errors added | pass, errors reported | **pass**; errors annotated | 245 s |
| [#8](https://github.com/valentin-best/pptctl-sample-decks/pull/8) | `.pre-commit-config.yaml` and a clean deck, committed through the hook | pass | **pass** | 241 s |

All eight conclusions matched. The time is almost all pptctl being built:
the sample repository's token can't read the private pptctl's releases, so the
Action logs `No release download for Linux/X64; building from source` and
compiles it (the Action step took 278 s of #1's 299 s). In #5 the Action step
took 9 s: it found no decks and installed nothing. Once pptctl is public, or
for a consumer with a token that can read its releases, the download replaces
the build.

## Annotations

Read back with the check-runs API, every lint finding is an annotation on the
deck it belongs to, titled `pptctl lint: <rule>`, with the slide or part in
the message:

- #4: 9 annotations on `decks/vendor-export.pptx`: `broken-rid`,
  `dangling-rel`, 2 × `shared-part`, `undeclared-ctype`, `unlisted-part` as
  errors; `ctype`, `orphan-part`, `undeclared-ctype` as warnings. Plus the
  step's own error, `lint found 6 error(s) and 3 warning(s) (fail-on: error)`.
- #6: 7 warnings on `decks/draft.pptx` (`alt-text`, `ctype`, `empty-ph`,
  `font-risk`, `img-bloat`, `overflow`, `tiny-text`), and the failure.
- #7: the same 9 as #4, and no failure.
- #1: the one `ctype` warning on `decks/roadmap.pptx`.

Annotations take a few seconds to appear after a run completes: one query made
right after #4 finished returned 7 of them, a later one all 9.

## The job summary

The Chrome extension wasn't connected, so the summary was checked another
way. `scripts/action/review.sh` was run locally on the same branches; its
outputs matched CI (#1: 0 errors, 1 warning; #4: 6 errors, 3 warnings). The
Markdown it wrote went through GitHub's own GFM renderer
(`gh api markdown -f mode=gfm`). Both summaries render as intended: the
overview and findings tables are real tables, one `<tr>` per row and no row
spilled into a paragraph. The diff is a highlighted `diff` block
(`highlight-source-diff`) inside an open `<details>`. #1's summary:

````
### pptctl: 1 deck changed

| Deck | Change | Lint |
|---|---|---|
| `decks/roadmap.pptx` | modified | 1 warning |

#### `decks/roadmap.pptx` (modified)

<details open><summary>Slide diff</summary>

```diff
--- a/decks/roadmap.pptx
+++ b/decks/roadmap.pptx
@@ slide 1 -> 1 (changed 0.89) @@
-Slide 1
+Slide 1 (revised)
@@ slide 12 -> 2 (moved) @@
@@ slide 3 -> 4 (changed 0.75) @@
  picture "Picture 2" replaced (image3.png 127 B 66x50 -> picture3.png 121 B 66x50)
@@ slide 5 -> 6 (changed 0.67) @@
  shape "Picture 2" (picture) removed
  picture "Picture 2" removed (image5.png 128 B 68x52)
@@ slide 7 -> 8 (changed 0.80) @@
  shape "Picture 3" (picture) added at (5.00 in, 1.50 in), 4.00 x 3.00 in
  picture "Picture 3" added (picture7.png 170 B 96x72)
@@ slide 9 -> 10 (changed 1.00) @@
  picture "Picture 2" alt text changed (alt text "image.png" -> "Slide 9 picture, described")
12 slides: 0 added, 0 removed, 1 moved, 5 changed
```

</details>

| Severity | Rule | Where | Message |
|---|---|---|---|
| warning | `ctype` | deck | part "ppt/printerSettings/printerSettings1.bin" has suspect content type "application/vnd.openxmlformats-officedocument.presentationml.printerSettings" |
````

The move, the text edit, every picture operation and the alt-text change are
there, each under its slide.

## `actions/checkout@v7`

Every run warned `Node.js 20 is deprecated. The following actions target
Node.js 20 …: actions/checkout@v4`, coming from the line our docs tell people
to copy. `actions/checkout@v5` and later run on Node 24. The current major,
v7, changes only fork checkouts under `pull_request_target`. Since v6 it keeps
credentials in a separate file, which matters because the Action runs
`git fetch` for the base commit. #1 was re-run with `@v7`: same result, the
base fetched, no deprecation warning. The README and
`docs/github-action.md` now say `@v7`.

## pre-commit, in the same repository

`.pre-commit-config.yaml` pointing at `https://github.com/valentin-best/pptctl`
with `rev:` the same `main` commit, installed with `pre-commit install`
(pre-commit 4.7.0), real `git commit`s:

| Hook | Commit | Result |
|---|---|---|
| `pptctl-lint` (pptctl on PATH) | deck with integrity errors | **blocked**, all 9 findings listed |
| `pptctl-lint` | a clean deck and a deck with warnings, together | **blocked**; both decks linted in one call, the clean one "no issues found" |
| `pptctl-lint` | the clean deck alone | **committed** (#8 contains it) |
| `pptctl-lint-fetch` (no pptctl on PATH) | deck with integrity errors | downloaded and checksum-checked pptctl 0.6.1 (through `gh`, the repository being private), then **blocked** |

pre-commit refuses to run while `.pre-commit-config.yaml` has unstaged
changes. Switching hooks means staging the config first. That is
pre-commit's rule, not ours.

`pptctl-lint-fetch` runs the release matching the hook's `rev`, and no release
includes the hooks yet, so at this commit it fetches 0.6.1, which takes one file
per call. From v0.7.0 on, `rev: v0.7.0` fetches a pptctl that takes several.

## After the v0.7.0 tag

A short re-check once the release exists: `uses: valentin-best/pptctl@v0.7.0`
on #1 (the Action then pins pptctl 0.7.0), and `pptctl-lint-fetch` at
`rev: v0.7.0` with two decks in one commit.
