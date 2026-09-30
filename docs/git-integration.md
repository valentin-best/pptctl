# Using `pptctl diff` with git

Out of the box git treats a deck as an opaque blob:

```
$ git diff HEAD~1 HEAD -- deck.pptx
diff --git a/deck.pptx b/deck.pptx
index 8cbd07b..54336be 100644
Binary files a/deck.pptx and b/deck.pptx differ
```

Two recipes fix that. Both hang off a git diff driver named `pptx`, assigned
to `.pptx` files with one line in `.gitattributes`:

```
*.pptx diff=pptx
```

Commit that file to the repo. The driver itself is per-user configuration
(`git config`, add `--global` to apply it to every repo), and `pptctl` has to
be on `PATH` when git runs.

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

Point the driver at [`scripts/git-pptx-diff.sh`](../scripts/git-pptx-diff.sh)
(an absolute path, or copy it somewhere on `PATH`):

```bash
git config diff.pptx.command "$PWD/scripts/git-pptx-diff.sh"
```

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

Neither recipe affects GitHub, GitLab or other hosted viewers; they run
their own diff. For a review there, attach the output of
`pptctl diff --format html old.pptx new.pptx > diff.html` to the pull request.
