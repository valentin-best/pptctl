# Using `pptctl diff` with git

Out of the box git treats a deck as an opaque blob:

```
$ git diff HEAD~1 HEAD -- deck.pptx
diff --git a/deck.pptx b/deck.pptx
index 8cbd07b..54336be 100644
Binary files a/deck.pptx and b/deck.pptx differ
```

Three recipes fix that: a text diff (textconv), the full `pptctl diff` in the
terminal (an external diff driver), and the HTML diff in a browser (a
difftool). The first two hang off a git diff driver named `pptx`, assigned to
`.pptx` files with one line in `.gitattributes`:

```
*.pptx diff=pptx
```

Commit that file to the repo. The driver itself is per-user configuration
(`git config`, add `--global` to apply it to every repo), and `pptctl` has to
be on `PATH` when git runs. To set up all three at once, see
[Setup in one go](#setup-in-one-go).

All output below comes from a scratch repo with two commits of `deck.pptx`:
`tests/fixtures/three_slides.pptx`, then a copy of it with slide 3 moved to
the front, slide 2 retitled and a fourth slide added.

## Recipe 1: textconv, text diffs everywhere

```bash
git config diff.pptx.textconv "pptctl extract --format txt"
```

```
$ git diff HEAD~1 HEAD -- deck.pptx
diff --git a/deck.pptx b/deck.pptx
index 8cbd07b..54336be 100644
--- a/deck.pptx
+++ b/deck.pptx
@@ -1,9 +1,12 @@
 slide 1
-  [ctrTitle] Slide 1
+  [ctrTitle] Slide 3
   [subTitle]
 slide 2
-  [ctrTitle] Slide 2
+  [ctrTitle] Slide 1
   [subTitle]
 slide 3
-  [ctrTitle] Slide 3
-  [subTitle]
+  [ctrTitle] Slide 2: revised title
+  [subTitle] New subtitle
+slide 4
+  [ctrTitle] Slide 4
+  [subTitle] Slide 4
```

A textconv filter gets **one** file and turns it into text; git then runs
its own line diff over the two conversions. `pptctl extract --format txt`
writes the text of every slide to stdout, so it slots in directly.

What you get: text changes wherever git shows a diff. `git diff`, `git log
-p`, `git show` and `git blame` all pick it up with no extra flags.

What you do not get: any of the semantics of `pptctl diff`. Git is comparing
lines of text, so it cannot tell a moved slide from a rewritten one. The
example above is exactly that: slide 3 moved to the front, which git reports
as three retitled slides. Shape geometry and pictures are not in the `txt`
dump at all, so a swapped image or a resized box is invisible.

Conversion runs every time git shows the file. To cache it in git notes:

```bash
git config diff.pptx.cachetextconv true
```

## Recipe 2: external diff driver, full semantic diff

Point the driver at [`scripts/git-pptx-diff.sh`](../scripts/git-pptx-diff.sh).
Every release archive carries a copy next to the `pptctl` binary; install both
somewhere on `PATH` (see [Installing](../README.md#installing)) and name the
script alone:

```bash
git config diff.pptx.command git-pptx-diff.sh
```

From a checkout of this repo instead, give the absolute path:
`git config diff.pptx.command "$PWD/scripts/git-pptx-diff.sh"`.

Git calls the script with seven arguments (`path old-file old-hex old-mode
new-file new-hex new-mode`); the script runs `pptctl diff` on `old-file` and
`new-file` and rewrites the file header to the path in the repo:

```
$ git diff HEAD~1 HEAD -- deck.pptx
--- a/deck.pptx
+++ b/deck.pptx
@@ slide 3 -> 1 (moved) @@
@@ slide 2 -> 3 (changed 0.50) @@
-Slide 2
+Slide 2: revised title
+New subtitle
@@ slide 4 (added) @@
4 slides: 1 added, 0 removed, 1 moved, 1 changed
```

Same commit, but now the move is a move, the retitle is a one-paragraph
change and the new slide is one line. Uncommitted changes work the same way
(`git diff`, `git diff --cached`). An added or deleted deck prints a single
line (`pptctl diff: new file second.pptx`) rather than diffing against
`/dev/null`.

Things to know:

- **The script always exits 0.** `pptctl diff` exits 1 whenever the decks
  differ, and git treats a non-zero exit from a diff driver as a failure:
  a naive `pptctl diff "$2" "$5"` driver prints the diff and then git aborts
  with `fatal: external diff died, stopping at deck.pptx`.
- **`git log -p` and `git show` ignore the driver unless you pass
  `--ext-diff`.** Without it they fall back to `Binary files ... differ`
  (or to textconv if that is configured too, see below). `git log -p
  --ext-diff` and `git show --ext-diff` run the script.
- `git diff --no-ext-diff` bypasses the driver for one run.
- Extra options reach `pptctl` through `PPTCTL_DIFF_ARGS`, for example
  `PPTCTL_DIFF_ARGS="--format json" git diff HEAD~1 HEAD -- deck.pptx` or
  `PPTCTL_DIFF_ARGS="--color"` to keep colour through git's pager.
- Colour is off by default because git pipes the driver's output through a
  pager, which is not a TTY as far as `pptctl` can tell.

For a one-off run without touching any config, `GIT_EXTERNAL_DIFF` names the
driver for that command:

```bash
GIT_EXTERNAL_DIFF="$PWD/scripts/git-pptx-diff.sh" git diff HEAD~1 HEAD -- deck.pptx
```

`GIT_EXTERNAL_DIFF` applies to every path in the diff, not only `.pptx`
files, so limit it with a pathspec (`-- '*.pptx'`) in a mixed repo.

## Recipe 3: difftool, the HTML diff in a browser

`pptctl diff --format html` renders a side-by-side page of every changed slide.
[`scripts/git-pptx-difftool.sh`](../scripts/git-pptx-difftool.sh), also in the
release archives, makes it a `git difftool`:

```bash
git config difftool.pptx.cmd 'git-pptx-difftool.sh "$LOCAL" "$REMOTE" "$MERGED"'
```

```
$ git difftool -y -t pptx HEAD~1 -- '*.pptx'
deck.pptx: /var/folders/…/T/pptctl-difftool/deck.KQ9WyZ.html
```

and the page opens in the default browser: `open` on macOS, `xdg-open` on
Linux, `start` on Windows. Set `PPTCTL_OPEN` to use another command
(`PPTCTL_OPEN=firefox`), or `PPTCTL_OPEN=none` to print the path only. The page
names the deck by its path in the repo, with `a/` and `b/` in front.

Things to know:

- **Keep the `-- '*.pptx'` pathspec.** `git difftool -t pptx` runs the tool on
  every changed file, and `pptctl` can't diff a `.md`.
- `-y` skips git's "Launch 'pptx'?" prompt for each file;
  `git config difftool.prompt false` does that for good.
- Pages are kept in `$TMPDIR/pptctl-difftool/` (each one is self-contained),
  because git deletes its temporary copies of the decks as soon as the tool
  returns. The system's temp-file cleanup removes them eventually.
- An added or deleted deck prints one line and opens nothing.
- `PPTCTL_DIFF_ARGS` passes options to `pptctl diff`, as for the driver.
- Unlike the other two recipes this needs no `.gitattributes` line: the tool
  is chosen with `-t pptx` on the command line.
- It works alongside the Recipe 2 driver. Git gives a diff driver precedence
  over any difftool, so `git-pptx-diff.sh` notices when it runs inside
  `git difftool` and hands over to the tool. Copies of the driver from v0.6.1
  and earlier don't; with one of those, `git difftool` prints the text diff
  instead, so update it.

## Setup in one go

With `pptctl`, `git-pptx-diff.sh` and `git-pptx-difftool.sh` on `PATH`
(they come in the same release archive):

```bash
git config --global diff.pptx.textconv "pptctl extract --format txt"
git config --global diff.pptx.command git-pptx-diff.sh
git config --global difftool.pptx.cmd 'git-pptx-difftool.sh "$LOCAL" "$REMOTE" "$MERGED"'
echo '*.pptx diff=pptx' >> .gitattributes    # in each repo, then commit it
```

Drop `--global` to keep it to one repo. To undo it, run
`git config --global --remove-section diff.pptx` and the same for
`difftool.pptx`.

## Which one should I use

Both, and let git pick. When `textconv` and `command` are both set on the
`pptx` driver, `git diff` runs the semantic driver and `git log -p` /
`git show` fall back to the textconv line diff; this was checked with the
repo above. That gives the full `pptctl diff` where you look at changes
most often, and a readable text diff everywhere else without remembering
`--ext-diff`.

If you can only have one: textconv is zero-maintenance and shows up in every
git command, so it is the better default for a repo where most people only
skim what changed. The external driver is the one to reach for when
slides get reordered, or when images and layout matter as much as the words.

The difftool is for when you want to *look*: a slide-by-slide view in the
browser, on demand, for the commits you ask about.

None of the recipes affect GitHub, GitLab or other hosted viewers; they run
their own diff. For pull requests on GitHub, use the
[GitHub Action](github-action.md), which puts the diff in the job summary and
can upload the HTML pages as an artifact.
