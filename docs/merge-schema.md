# `pptctl merge` JSON Output Schema

**Current version: 1**

`pptctl merge --json A.pptx B.pptx [C.pptx …] (-o OUT | --dry-run) [--theme=N]`
emits a single JSON object to stdout describing what each input contributed to
the merged deck. This document is the authoritative spec for that output.
Downstream tools (pipelines that assemble decks, CI checks, golden tests)
should key off `schema_version` to guard against breaking changes.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or meaning, changing the ordering guarantee) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new warning texts) do not bump
  the version; consumers should ignore unknown fields.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `output` | `string` \| `null` | The `-o` path the merged deck was written to; `null` for `--dry-run`. |
| `dry_run` | `boolean` | `true` when nothing was written. |
| `theme` | `integer` \| `null` | The `--theme=N` deck (1-based) every slide was restyled onto; `null` when each deck kept its own formatting (the default). |
| `slides` | `integer` | Slides in the merged deck. |
| `output_bytes` | `integer` | Size of the merged file, in bytes. Computed on a dry run too (the result is built in a temporary file and measured). |
| `inputs` | `array<Input>` | One entry per input, **in input order**, which is also the slide order of the output. |

---

## Input

| Field | Type | Description |
|---|---|---|
| `path` | `string` | The path as given on the command line. |
| `base` | `boolean` | `true` for the one input the output is built on: the `--theme` deck, else the first. Its parts are copied byte for byte; everything else arrives as added parts. |
| `bytes` | `integer` | Size of the input file, in bytes. |
| `slides` | `integer` | Slides this input contributed to the output. For the base, all of its slides. |
| `parts_added` | `integer` | Parts copied into the output from this input (slides, notes, charts, media, masters, layouts, themes, …), not counting `.rels` parts. `0` for the base. |
| `media_deduplicated` | `object` | Media this input shared with the output instead of copying, because a byte-identical part was already there: `count` (parts) and `bytes` (their total uncompressed size). |
| `masters` | `object` | Slide masters: `imported` (copied, with their layouts and theme) and `reused` (byte-identical to a master already in the output, so this input's slides use that one instead). Both `0` for the base, and for every input under `--theme`. |
| `warnings` | `array<string>` | What didn't carry over cleanly, in human-readable form. Empty when nothing did. See [Warnings](#warnings). |

### Warnings

The texts are for people and may change between versions without a schema
bump. Current ones:

| Warning | Meaning |
|---|---|
| `slide size W x H in differs from the base deck's …` | The input's slides keep their geometry, unscaled, on the base's canvas. |
| `slide N: deck K has no layout like "X"; placed on "Y"` | `--theme` only: no layout in deck K has slide N's layout type or name, so it uses the fallback (Title and Content). |
| `…: relationship rIdN targets …, which is missing` | The input itself has a dangling relationship; it is copied as is. |
| `…: no content type declared` | The input itself has a part with no content type; it is copied without one. |
| `comment author N isn't in the deck's commentAuthors.xml; …` | The input's comments name an author it doesn't list; they are attributed to "Unknown author". |
| `the deck's modern comments have no authors.xml; …` | Likewise for modern (PowerPoint 365) comments. |
| `a default text style that differs from the base deck's (level 1: 14 pt vs 18 pt; …): its text boxes …` | Text boxes that set no size, font or spacing of their own fall back to the presentation's `p:defaultTextStyle`, and the output keeps only the base's. The parenthesis names the level-1 size and font where both decks set them and they differ. Proofing language (`lang`) and tab spacing (`defTabSz`) are ignored. Not reported under `--theme`. |
| `no default text style, unlike the base deck: …` / `a default text style the base deck lacks: …` | The same, when one of the two has none (LibreOffice writes none). |
| `added p:clrMapOvr to N notes slide(s) that lacked it (Keynote refuses to open a deck without it)` | The input's notes slides (LibreOffice and ONLYOFFICE write them this way) were given the colour-map override PowerPoint writes; nothing else in them changed. Also reported for the base deck. |

---

## Example

`pptctl merge --json --dry-run merge_a.pptx merge_b.pptx merge_c.pptx` on
the test fixtures (`merge_a` and `merge_c` share a template and a logo,
`merge_b` has its own theme):

```json
{
  "dry_run": true,
  "inputs": [
    {
      "base": true,
      "bytes": 30592,
      "masters": { "imported": 0, "reused": 0 },
      "media_deduplicated": { "bytes": 0, "count": 0 },
      "parts_added": 0,
      "path": "merge_a.pptx",
      "slides": 3,
      "warnings": []
    },
    {
      "base": false,
      "bytes": 41221,
      "masters": { "imported": 1, "reused": 0 },
      "media_deduplicated": { "bytes": 0, "count": 0 },
      "parts_added": 21,
      "path": "merge_b.pptx",
      "slides": 2,
      "warnings": []
    },
    {
      "base": false,
      "bytes": 29826,
      "masters": { "imported": 0, "reused": 1 },
      "media_deduplicated": { "bytes": 143, "count": 1 },
      "parts_added": 2,
      "path": "merge_c.pptx",
      "slides": 2,
      "warnings": []
    }
  ],
  "output": null,
  "output_bytes": 67288,
  "schema_version": 1,
  "slides": 7,
  "theme": null
}
```

Keys are emitted in sorted order (as shown, nested objects compacted here for
reading) because the serializer uses an ordered map; consumers should not
depend on key order.
