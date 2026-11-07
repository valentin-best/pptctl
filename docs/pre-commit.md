# Linting decks before a commit (pre-commit)

pptctl ships two [pre-commit](https://pre-commit.com) hooks that run
`pptctl lint --strict` on every `.pptx` a commit adds or changes, and stop the
commit when lint finds anything. [prek](https://github.com/j178/prek) reads the
same configuration.

| Hook | Runs | Needs |
|---|---|---|
| `pptctl-lint` | the `pptctl` on `PATH` | `pptctl` installed (see [Installing](../README.md#installing)) |
| `pptctl-lint-fetch` | the pptctl release matching the hook's `rev`, downloaded on first use | bash and curl (Git for Windows provides both) |

Use `pptctl-lint` when everyone who commits has pptctl installed, and
`pptctl-lint-fetch` when they don't, or when the whole team should run the same
pptctl version without thinking about it.

## Setup

`.pre-commit-config.yaml` at the root of the repository that holds the decks:

```yaml
repos:
  - repo: https://github.com/valentin-best/pptctl
    rev: v0.8.0
    hooks:
      - id: pptctl-lint
```

Then, once per clone:

```bash
pre-commit install
```

A commit that adds a deck with findings is refused:

```
$ git commit -m "add decks"
pptctl lint..............................................................Failed
- hook id: pptctl-lint
- exit code: 2

deck.pptx: warning[font-risk]: font "Nonexistent Sans" is referenced but not embedded and not a system or Office font
deck.pptx: warning[ctype]: part "ppt/printerSettings/printerSettings1.bin" has suspect content type "application/vnd.openxmlformats-officedocument.presentationml.printerSettings"
deck.pptx:1: warning[overflow]: shape "TextBox 1" extends 2.00 in past the right edge
deck.pptx:2: warning[tiny-text]: shape "TextBox 1" has text at 8.0pt (below 12pt minimum)
deck.pptx:3: warning[alt-text]: picture "Picture 1" has no alt text
deck.pptx:5: warning[empty-ph]: placeholder "Title 1" (title) is empty
deck.pptx:7: warning[img-bloat]: picture "Picture 1" is 200x150 px shown at 0.5x0.4 in (400 DPI effective, above 300 DPI, 88.1 KiB stored)
deck.pptx: 7 warnings
title.pptx: no issues found
```

Fix the deck and commit again, or skip the hooks for one commit with
`git commit --no-verify`. To lint every deck already in the repository, not
only staged ones: `pre-commit run pptctl-lint --all-files`.

## Choosing what blocks a commit

By default every rule runs and any finding blocks, warnings included. To be
less strict, give the hook its own `args`. They **replace** the default
`[--strict]`, so repeat it: without `--strict`, `pptctl lint` exits 0 and the
hook never blocks anything.

Only package-integrity problems, the ones that make PowerPoint offer a repair
or Keynote refuse the file:

```yaml
      - id: pptctl-lint
        args: [--strict, --only, integrity]
```

Integrity plus a few content rules, with a lower text-size floor:

```yaml
      - id: pptctl-lint
        args: [--strict, --only, 'integrity,overflow,alt-text', --min-font-size, '10']
```

Every rule id and option is listed under [`pptctl lint`](../README.md#pptctl-lint).

To skip some decks, use pre-commit's own `exclude` (a Python regex):

```yaml
      - id: pptctl-lint
        exclude: ^archive/
```

## `pptctl-lint-fetch`

```yaml
      - id: pptctl-lint-fetch
```

It runs the release whose version matches the hook's `rev` (`rev: v0.8.0`
runs pptctl 0.8.0, even if another pptctl is on `PATH`). On first use it
downloads that release's archive for the machine (Linux x86_64 or aarch64,
macOS arm64, Windows x86_64), checks it against the release's `SHA256SUMS`, and
unpacks it into `~/.cache/pptctl/` (`$XDG_CACHE_HOME/pptctl/` when that is set).
Later runs use the cached copy and need no network.

Downloads use `curl`. If that fails and the [GitHub CLI](https://cli.github.com)
is installed, `gh release download` is tried with its login, which is how a
private fork's releases are reached.

Environment variables, all optional: `PPTCTL_VERSION` (a release tag to use
instead of the hook's), `PPTCTL_CACHE_DIR`, and `PPTCTL_REPO` (the repository
releases come from; default `valentin-best/pptctl`).

There is no release for Intel Macs; use `pptctl-lint` with a pptctl built from
source there.

## Not included

There is no `optimize` hook: a hook that rewrites decks during a commit tends
to surprise people. Run `pptctl optimize` yourself, or ask for one in an issue.
