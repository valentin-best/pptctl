# The pptctl GitHub Action

Add one workflow file and every pull request that touches a `.pptx` gets:

- a **job summary** with one row per changed deck, a `pptctl diff` of each
  modified or renamed deck against the base, and a table of lint findings;
- an **annotation** per lint finding on the deck it belongs to, listed on the
  run and on the pull request's checks;
- a **failed check** when lint finds errors (configurable with `fail-on`).

Pull requests that don't touch a deck finish in seconds: the Action looks for
changed decks before it downloads anything.

## Setup

`.github/workflows/decks.yml`:

```yaml
name: decks
on:
  pull_request:
    paths: ['**.pptx', '**.PPTX']

jobs:
  review:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: valentin-best/pptctl@main
```

Nothing else is needed. The Action is new on `main`; pin a release tag
(`@v0.7.0` and later) once one includes it.

`actions/checkout`'s default depth of 1 is fine: the Action fetches the base
commit itself.

The `paths` filter on the trigger is optional. Without it the workflow still
runs on every pull request and ends straight away when no deck changed.

Decks stored with Git LFS work as long as `git lfs` is installed (it is on
GitHub-hosted runners): the Action reads both versions through `git lfs
smudge`, which fetches them, so `lfs: true` on the checkout is not required.

## What the summary looks like

For a pull request that edits one deck, deletes one, renames one and adds two:

```
### pptctl: 5 decks changed

| Deck                  | Change                              | Lint                 |
|-----------------------|-------------------------------------|----------------------|
| `clean.pptx`          | added                               | clean                |
| `decks/Bad.PPTX`      | added                               | 6 errors, 3 warnings |
| `decks/a.pptx`        | modified                            | 1 warning            |
| `decks/gone.pptx`     | deleted                             | —                    |
| `decks/new-name.pptx` | renamed from `decks/old name.pptx`  | 1 warning            |
```

followed by a section per deck, for example:

````
#### `decks/a.pptx` (modified)

```diff
--- a/decks/a.pptx
+++ b/decks/a.pptx
@@ slide 2 -> 2 (changed 0.89) @@
-Review of last quarter
+Review of this quarter
+Closing remarks
@@ slide 5 -> 5 (changed 0.92) @@
-Remember to pause for questions.
+Remember to pause for discussion.
6 slides: 0 added, 0 removed, 0 moved, 3 changed
```

| Severity | Rule   | Where | Message                                  |
|----------|--------|-------|------------------------------------------|
| warning  | `ctype`| deck  | part "ppt/printerSettings/…" has suspect content type … |
````

A diff longer than 400 lines is cut, with a note; GitHub caps a step's
summary at 1 MiB. A deck whose slide content didn't change (a rename, or an
edit to something `pptctl diff` doesn't compare) says so instead of showing
an empty diff.

## Inputs

| Input | Default | |
|---|---|---|
| `version` | the Action's tag, if it is a release tag; else the newest release | pptctl release to download, e.g. `v0.6.1`. |
| `paths` | whole repository | Git pathspecs, one per line, limiting which decks are reviewed (`decks/`, `slides/*.pptx`). |
| `base` | the pull request's base, or the commit before a push | Commit to compare against. Without one (a push that creates a branch), every deck under `paths` is linted and none is diffed. |
| `head` | `HEAD` | Commit to review. |
| `lint-args` | none | Extra `pptctl lint` arguments, e.g. `--only integrity` or `--target windows`. |
| `diff-args` | none | Extra `pptctl diff` arguments, e.g. `--strict-text` or `--format json`. |
| `fail-on` | `error` | `error`: fail on any lint error, or a deck that can't be read. `warning`: also on warnings. `never`: report only. |
| `html-diff` | `false` | Also upload `pptctl diff --format html` pages, one per diffed deck, as the `pptctl-diff` workflow artifact. |
| `working-directory` | `.` | The repository to review, when it isn't checked out at the workspace root. |
| `token` | `github.token` | Token for downloading the pptctl release. |

## Outputs

| Output | |
|---|---|
| `decks-changed` | Decks added, modified, renamed or deleted. |
| `lint-errors` | Lint findings with severity `error`, plus decks that couldn't be read. |
| `lint-warnings` | Lint findings with severity `warning`. |

Content rules (`overflow`, `tiny-text`, `alt-text`, …) report warnings, so with
the default `fail-on: error` they show up in the summary without failing the
check; the package-integrity rules report the errors that do (see
[`lint-schema.md`](lint-schema.md)). Use `fail-on: warning` to gate on both.

Lint runs on every deck that exists after the change, not only on what the
change touched in it: an unchanged warning in a deck someone edits will show
up. Narrow it with `lint-args` (`--only integrity`) if that is noise.

## How pptctl gets installed

The Action downloads the release archive for the runner (Linux x86_64 and
arm64, macOS arm64, Windows x86_64), checks it against the release's
`SHA256SUMS` and puts `pptctl` on `PATH` for the rest of the job. Pinning the
Action to a release tag (`uses: valentin-best/pptctl@v0.7.0`) pins pptctl to
the same release; otherwise set the `version` input.

When there is no archive for the runner, or the download is refused, the
Action builds pptctl from its own source instead (Linux and macOS runners;
a few minutes). That is what happens while the pptctl repository is private
and the workflow runs in another repository: its `github.token` can't read
pptctl's releases. To download instead, pass a token that can as `token`.
A private action is only usable from other repositories of the same owner
once pptctl's *Settings › Actions › General › Access* allows it.

## Running it by hand

The Action is `scripts/action/review.sh`, configured through environment
variables (see the header of the script). In the repository to review, with
`pptctl` on `PATH`:

```bash
export PPTCTL_WORK=$(mktemp -d) PPTCTL_BASE=origin/main
review=/path/to/pptctl/scripts/action/review.sh
"$review" detect && "$review" review
```

prints the outputs, the annotations and the Markdown summary to stdout.
