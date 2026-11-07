---
name: pptctl
description: Check, compare, shrink and combine PowerPoint .pptx files with the pptctl CLI. Use whenever you create or modify a .pptx (python-pptx, PptxGenJS, OfficeCLI, LibreOffice, or by hand), before handing a deck to anyone, when asked what changed between two decks, to make a deck smaller, to merge decks, or to read a deck's text.
---

# pptctl: verify decks before anyone opens them

`pptctl` is a command-line tool for `.pptx` files. It does not create or
edit slide content: tools like python-pptx do that. It checks what they wrote
(`lint`), shows what an edit changed (`diff`), shrinks a deck (`optimize`),
combines decks without breaking their pictures and charts (`merge`), and dumps
the text (`extract`).

Decks written by libraries often look fine to the library and still make
PowerPoint offer a "repair", or Keynote refuse the file. Nothing in your own
code will tell you; `pptctl lint` will.

Check it is installed with `pptctl --version`. If it isn't, tell the user
(releases: https://github.com/valentin-best/pptctl/releases) rather than
skipping the checks silently.

## The rule

**After you write or change any `.pptx`, and before you report the task done:**

```bash
pptctl lint --strict --only integrity out.pptx   # must exit 0
pptctl lint out.pptx                             # then read the content findings
```

The first command checks the package (parts, relationships, content types).
Any finding there is a broken file: fix it before handing the deck over.
The second runs every rule; its warnings are about what the slides show
(text off the slide, tiny text, missing alt text). Fix the ones your change
caused; mention the rest to the user rather than rewriting their deck.

## Exit codes

Read the exit code before the output. Several commands return non-zero for
results that are not errors.

| Command | 0 | 1 | 2 |
|---|---|---|---|
| `lint` | read; findings may exist unless `--strict` | file can't be read as a `.pptx` | `--strict` and something was found |
| `diff` | decks are identical | **decks differ (normal result, not an error)** | a file can't be read |
| `optimize` | optimized or measured (even if nothing changed) | can't read input / write output, or no output option | — |
| `merge` | merged or measured | an input can't be read, `--theme` out of range, `-o` names an input, can't write | — |
| `info`, `extract` | ok | file can't be read | — |

Any other non-zero code is a command-line mistake (unknown flag, missing
file); the usage message is on stderr.

`lint` accepts several files and exits with the worst code. `--json` takes one
file per call.

## Workflows

### 1. Verify a deck you (or another tool) wrote

```bash
pptctl lint --strict --only integrity deck.pptx
pptctl lint --json deck.pptx
```

`--json` gives one object: `diagnostics` (each with `rule`, `severity`,
`slide` (1-based or null), `shape`, `part`, `message`) and `summary` with
`errors` and `warnings`. Key off `rule`, not `message` (wording changes
between versions). Rules and their usual fixes are in the table at the end.

Lint again after every fix until the integrity run exits 0.

### 2. See what an edit actually changed

```bash
pptctl diff before.pptx after.pptx                # readable summary
pptctl diff --format json before.pptx after.pptx  # to check programmatically
```

Use this to confirm your edit did what was asked and nothing else: copy the
deck before editing, diff afterwards, and read every hunk. Slides are matched
by content, so a reordered slide shows as `moved`, not as changed text.
Unified output: `@@ slide 2 -> 3 (changed 0.50) @@` per slide, `-`/`+` for
text, `~` for formatting, indented lines for shape and picture changes, and a
final count line. JSON: `slides[]` with `status` (`unchanged`, `changed`,
`added`, `removed`), `a_index`, `b_index`, `moved`, and `changes` (`text`,
`shapes`, `pictures`, `tables`, `charts`); `summary` has the counts.

Exit code 1 here means "they differ". Don't report it as a failure, and
don't retry.

### 3. Make a deck smaller

```bash
pptctl optimize --dry-run deck.pptx         # what it would save; writes nothing
pptctl optimize -o deck-small.pptx deck.pptx
pptctl lint --strict --only integrity deck-small.pptx
```

Five passes, each with an opt-out: unused layouts and masters
(`--no-layouts`), embedded fonts (`--keep-fonts`), picture regions cropped
out of view (`--no-crop`), images stored bigger than they are shown
(`--no-images`), unreferenced media (`--no-orphans`). It never makes a part
bigger, and leaves vector images, video and audio alone. Use `--keep-fonts`
when the deck relies on an embedded font the recipient won't have.

`--json` reports `input_bytes`, `output_bytes` and per-pass `actions`. Tell
the user the before and after sizes. `-o` must name a new file; `--in-place`
overwrites the input, so use it only when asked.

### 4. Combine decks

```bash
pptctl merge a.pptx b.pptx c.pptx -o combined.pptx
pptctl lint --strict --only integrity combined.pptx
```

Slides come out in input order. Each deck keeps its own look by default;
`--theme=N` (1-based) puts every slide on deck N's layouts so the result looks
like one deck, at the cost of fonts, colours and placeholder positions
changing on the others. Report the `warnings` the command prints (different
slide sizes, layout fallbacks, a different default text style, charts Google
Slides will recolour) to the user. `--dry-run` measures without
writing; `--json` gives a per-input report.

Use `pptctl merge` instead of copying slide XML between decks yourself with
python-pptx. A copied `<p:pic>` keeps an `r:embed="rIdN"` that means nothing in
the target deck, so pictures, charts and notes break.

### 5. Read a deck without rendering it

```bash
pptctl extract --format md deck.pptx    # every slide's text, with placeholder roles
pptctl extract deck.pptx                # same as JSON
pptctl info deck.pptx                   # slide count, size, fonts, theme colours, media
```

This is quicker than unzipping and reading XML, and it resolves placeholders
the way PowerPoint shows them.

## Lint rules and the usual fix

Package integrity (`--only integrity`). These are errors in the file itself.
They come from the tool that wrote it, so fix the generating code, not the
zip:

| Rule | Meaning | Usual fix |
|---|---|---|
| `dangling-rel` | A relationship points at a part that doesn't exist. | Something deleted or renamed a part without removing its relationship. Remove the reference in the code that deletes the part. |
| `broken-rid` | An `r:embed` / `r:id` names an id the part's `.rels` doesn't define. | XML was copied between parts or decks. Use `pptctl merge` to move slides between decks, or create the relationship with the library (`part.relate_to(...)`). |
| `undeclared-ctype` | A part has no content type in `[Content_Types].xml`. | Add a `Default` for the extension or an `Override` for the part. Usually from writing a new part (e.g. `.webp` media) behind the library's back. |
| `orphan-part` | No relationship reaches a part. | Usually harmless leftovers. `pptctl optimize` removes orphaned media. |
| `shared-part` | A notes slide, chart or comment part is used by several slides. | Each slide needs its own copy. Typical of duplicating a slide by copying its relationships. |
| `unlisted-part` | The presentation relates a slide or master but doesn't list it in `presentation.xml`. | See python-pptx below for the notes-master case. For a slide, add it to `p:sldIdLst` (the library's `add_slide` does this). |
| `notes-clrmap` | A speaker-notes slide has no `p:clrMapOvr`; Keynote refuses the deck. | Typical of decks saved by LibreOffice or ONLYOFFICE. Add `<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>` as the last child of `p:notes` (before any `p:extLst`). `pptctl merge` adds it to every notes slide it writes. |

Content rules (warnings):

| Rule | Meaning | Usual fix |
|---|---|---|
| `overflow` | A shape extends past the slide edge. | Move or resize it inside the slide size (`prs.slide_width`, `prs.slide_height`). Check text boxes you positioned by hand. |
| `tiny-text` | Text below 12 pt (`--min-font-size`). | Larger font, less text per slide, or split the slide. |
| `alt-text` | A picture without alt text. | Set a description (python-pptx: `pic._element.nvPicPr.cNvPr.set("descr", "…")`). |
| `font-risk` | A font that is neither embedded nor installed on Windows and macOS. | Use a common font (Calibri, Arial, Georgia, …) unless the user chose this one. The message says what it falls back to. |
| `empty-ph` | A content placeholder left empty (shows "Click to add text" while editing). | Fill it, or remove it (python-pptx: `ph._element.getparent().remove(ph._element)`). |
| `ctype` | A part with a content type renderers choke on, usually the printer-settings part. | See python-pptx below. |
| `img-bloat` | An image stored at far more resolution than it is displayed. | `pptctl optimize`, or resize the image before inserting it. |

## python-pptx: two fixes for every deck it writes

python-pptx's default template contains a printer-settings part (`ctype`),
and a deck with speaker notes relates a notes master without listing it
(`unlisted-part`). Keynote refuses the second outright. Call this before
`prs.save(...)`:

```python
from pptx.opc.constants import RELATIONSHIP_TYPE as RT
from pptx.oxml.ns import qn
from pptx.oxml.xmlchemy import OxmlElement


def tidy_for_pptctl(prs):
    """Fix the two problems python-pptx leaves in decks it writes."""
    pres = prs.part._element
    # A deck with speaker notes: list the notes master python-pptx related.
    related = any(r.reltype == RT.NOTES_MASTER for r in prs.part.rels.values())
    if related and pres.find(qn("p:notesMasterIdLst")) is None:
        rid = prs.part.relate_to(prs.notes_master.part, RT.NOTES_MASTER)
        lst = OxmlElement("p:notesMasterIdLst")
        entry = OxmlElement("p:notesMasterId")
        entry.set(qn("r:id"), rid)
        lst.append(entry)
        pres.find(qn("p:sldMasterIdLst")).addnext(lst)
    # The default template's printer settings.
    for rid, rel in list(prs.part.rels.items()):
        if rel.reltype == RT.PRINTER_SETTINGS:
            prs.part.drop_rel(rid)
```

Then lint the saved file as usual.

## Don't

- Don't treat `diff` exit 1 as an error.
- Don't fix integrity findings by unzipping and hand-editing XML unless you
  also update the `.rels` and `[Content_Types].xml` entries involved; lint
  again afterwards either way.
- Don't silence findings by narrowing `--only` to make a check pass. Narrow
  it to separate "broken file" from "looks wrong", as in workflow 1.
- Don't `optimize --in-place` or overwrite a user's deck unless asked; write
  a new file and say where it is.
