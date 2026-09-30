# `pptctl diff` JSON Output Schema

**Current version: 1**

`pptctl diff --format json A B` emits a single JSON object to stdout: the full
slide mapping between the two decks, including unchanged slides, so a tool can
reconstruct where every slide went. This document is the authoritative spec
for that output. Downstream tools (review bots, CI gates, golden tests) should
key off `schema_version` to guard against breaking changes.

The unified (`--format unified`, the default) and HTML (`--format html`)
renderers are views of the same report; only the JSON shape is versioned.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or meaning, changing the ordering guarantee) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new op kinds inside `changes`)
  do not bump the version; consumers should ignore unknown fields and unknown
  op kinds.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `a` | `string` | Path of the old deck, as passed to the command. |
| `b` | `string` | Path of the new deck, as passed to the command. |
| `slides` | `array<Slide>` | One entry per slide of A ∪ B, in the order described below. |
| `summary` | `Summary` | Counts by status. |

### Ordering

Entries follow the order of deck B. A slide that exists only in A (`removed`)
is slotted at the position it had in A, so reading top to bottom walks the
new deck while still showing where old slides disappeared. The order is
stable across runs: the document is byte-for-byte reproducible for a given
pair of decks and `pptctl` version.

---

## Slide

| Field | Type | Description |
|---|---|---|
| `status` | `"unchanged"` \| `"changed"` \| `"added"` \| `"removed"` | How the slide was classified (see below). |
| `a_index` | `integer` \| `null` | 1-based position in A. `null` when `status` is `added`. |
| `b_index` | `integer` \| `null` | 1-based position in B. `null` when `status` is `removed`. |
| `moved` | `boolean` | `true` for a matched slide whose order relative to its peers changed. Always `false` for `added` / `removed`. |
| `similarity` | `number` | Token similarity in `[0, 1]`: `1` for an exact fingerprint match, the best-match score for `changed`, `0` for `added` / `removed`. |
| `changes` | `Changes` | Nested ops. Non-empty only when `status` is `changed`. |

### Status

| `status` | Meaning |
|---|---|
| `unchanged` | Same content fingerprint in both decks. May still be `moved`. |
| `changed` | No exact match, but paired with its most similar counterpart above `--match-threshold` (default `0.5`). `changes` says what differs. |
| `added` | Present in B only. |
| `removed` | Present in A only. |

---

## Changes

| Field | Type | Description |
|---|---|---|
| `text` | `array<TextChange>` | Text-level ops within matched shapes and the notes. |
| `shapes` | `array<ShapeChange>` | Structural shape ops: added, removed, moved, resized, restyled. |
| `pictures` | `array<PictureChange>` | Picture media ops: added, removed, replaced, re-linked, alt-text. |
| `tables` | `array<TableChange>` | Table ops: cell text, rows/columns added or removed, merge changes. |
| `charts` | `array<ChartChange>` | Chart ops: type, title, series added/removed, categories, values. |

All five arrays are always present (empty for slides that are not
`changed`). Every key of an op is always present; a key that does not apply
to that op is `null`. Adding a new op kind or key is an additive change.

### TextChange

| Field | Type | Description |
|---|---|---|
| `kind` | `string` | `inserted`, `deleted`, `changed` or `reformatted`. |
| `shape` | `string` | `p:cNvPr@name` of the shape in B; `""` for the notes. |
| `shape_id` | `string` | `p:cNvPr@id` of the shape in B; `""` for the notes. |
| `notes` | `boolean` | `true` when the change is in the speaker notes. |
| `a_paragraph` | `integer \| null` | 1-based paragraph index in A; `null` when `inserted`. |
| `b_paragraph` | `integer \| null` | 1-based paragraph index in B; `null` when `deleted`. |
| `old_text` | `string \| null` | A paragraph text; `null` when `inserted`. |
| `new_text` | `string \| null` | B paragraph text; `null` when `deleted`. |
| `formatting` | `string \| null` | `reformatted` only: what changed, e.g. `"world": bold on, size 18pt -> 24pt`. |

Whitespace-only differences and field runs (`a:fld`) are ignored unless
`--strict-text` is given.

### ShapeChange

| Field | Type | Description |
|---|---|---|
| `op` | `string` | `added`, `removed`, `moved`, `resized`, `kind_changed` or `recolored`. One shape can yield several ops. |
| `id` | `string` | `p:cNvPr@id`, from B (from A when `removed`). |
| `name` | `string` | `p:cNvPr@name`, same side as `id`. |
| `kind` | `string` | Shape kind in B (in A when `removed`); the new kind for `kind_changed`. |
| `group_path` | `array<string>` | Names of enclosing groups, outermost first. |
| `old_kind` | `string \| null` | `kind_changed` only. |
| `old_geometry` | `Geometry \| null` | A side; `null` when `added` or A has no `a:xfrm`. |
| `new_geometry` | `Geometry \| null` | B side; `null` when `removed` or B has no `a:xfrm`. |
| `delta` | `Geometry \| null` | `moved` / `resized`: new minus old. |
| `old_fill` | `Fill \| null` | A side; `null` when inherited. |
| `new_fill` | `Fill \| null` | B side; `null` when inherited. |

`Geometry` carries EMU as integers (`x_emu`, `y_emu`, `cx_emu`, `cy_emu`)
alongside inches as doubles (`x_in`, `y_in`, `cx_in`, `cy_in`). `Fill` is
`{ "kind": "solid" | "none", "color": "<RRGGBB or scheme name>" }`. Moves and
resizes at or below `--geometry-tolerance` EMU (default `9525`, one pixel at
96 DPI) are not reported.

### PictureChange

| Field | Type | Description |
|---|---|---|
| `op` | `string` | `added`, `removed`, `replaced`, `relinked` or `alt_text_changed`. |
| `shape_id` | `string` | `p:cNvPr@id`, from B when the shape exists there, else A. |
| `shape_name` | `string` | `p:cNvPr@name`, same side as `shape_id`. |
| `before` | `Picture \| null` | A side; `null` when `added`. |
| `after` | `Picture \| null` | B side; `null` when `removed`. |

`Picture` has `rel_id`, `linked`, `target`, `content_type`, `size_bytes`,
`media_hash` (16 hex digits, FNV-1a 64 of the media bytes; `null` for linked
or unresolved images), `width`, `height` (pixels when the header could be
probed, else `null`), `alt_text`, `title`, `decorative`. Two pictures are
the same media when their `media_hash` values are equal, regardless of the
part name.

### TableChange

| Field | Type | Description |
|---|---|---|
| `op` | `string` | `cell_changed`, `rows_added`, `rows_removed`, `columns_added`, `columns_removed` or `merge_changed`. |
| `shape_id` | `string` | `p:cNvPr@id` of the table in B. |
| `shape_name` | `string` | `p:cNvPr@name` of the table in B. |
| `a_row` | `integer \| null` | `cell_changed` / `merge_changed`: 1-based row in A. |
| `b_row` | `integer \| null` | `cell_changed` / `merge_changed`: 1-based row in B. |
| `col` | `integer \| null` | `cell_changed` / `merge_changed`: 1-based column. |
| `indices` | `array<integer> \| null` | `rows_*` / `columns_*`: the affected 1-based indices (B side when added, A side when removed). |
| `old_text` | `string \| null` | `cell_changed`: A cell text, paragraphs joined by `\n`. |
| `new_text` | `string \| null` | `cell_changed`: B cell text. |
| `old_merge` | `Merge \| null` | `merge_changed`: A side. |
| `new_merge` | `Merge \| null` | `merge_changed`: B side. |

`Merge` is `{ "col_span", "row_span", "covered" }` (`gridSpan`, `rowSpan`,
and whether the cell is covered by a neighbour's `hMerge` / `vMerge`).
Tables are paired by shape id, then name. Column counts are compared first
and only the common prefix of columns is compared cell by cell; rows are
aligned by their cell text, so an inserted row is one `rows_added` op and
does not turn the rows below it into changed cells. Cell text follows the
same whitespace and field-run rules as `TextChange` (`--strict-text`).

### ChartChange

| Field | Type | Description |
|---|---|---|
| `op` | `string` | `type_changed`, `title_changed`, `series_added`, `series_removed`, `categories_changed`, `values_changed` or `more_values_changed`. |
| `shape_id` | `string` | `p:cNvPr@id` of the chart in B. |
| `shape_name` | `string` | `p:cNvPr@name` of the chart in B. |
| `series` | `string \| null` | Series name for series-level ops; `null` for `type_changed` / `title_changed`. |
| `series_index` | `integer \| null` | 1-based series position in B (in A for `series_removed`). |
| `point` | `integer \| null` | `values_changed`: 1-based point index. |
| `category` | `string \| null` | `values_changed`: the point's category label (`""` when the series has none). |
| `old_value` | `number \| null` | `values_changed`: A value; `null` when the point only exists in B. |
| `new_value` | `number \| null` | `values_changed`: B value; `null` when the point only exists in A. |
| `old_type` | `string \| null` | `type_changed`: A chart type (`bar`, `line`, `pie`, `bar+line`, …). |
| `new_type` | `string \| null` | `type_changed`: B chart type. |
| `old_title` | `string \| null` | `title_changed`: A title; `null` when A had none. |
| `new_title` | `string \| null` | `title_changed`: B title; `null` when B has none. |
| `old_categories` | `array<string> \| null` | `categories_changed`: A labels. |
| `new_categories` | `array<string> \| null` | `categories_changed`: B labels. |
| `more` | `integer \| null` | `more_values_changed`: further differing points of the series not listed. |

Charts are paired by shape id, then name. Series are matched by name when
every series on both sides has a distinct, non-empty name, otherwise by
position. Values compare exactly. At most 50 `values_changed` ops are
emitted per series; the remainder collapse into one `more_values_changed`.

---

## Summary

| Field | Type | Description |
|---|---|---|
| `unchanged` | `integer` | Number of slides with `status: "unchanged"`. |
| `changed` | `integer` | Number of slides with `status: "changed"`. |
| `added` | `integer` | Number of slides with `status: "added"`. |
| `removed` | `integer` | Number of slides with `status: "removed"`. |
| `moved` | `integer` | Number of slides with `moved: true` (counted across `unchanged` and `changed`). |

---

## Exit codes

Exit codes follow `git diff` and are the same in every format and under
`--quiet` (which suppresses stdout entirely).

| Code | Meaning |
|---|---|
| `0` | The decks are identical: every slide is `unchanged` and none is `moved`. |
| `1` | Differences were found. |
| `2` | Error: a file could not be opened or is not a valid `.pptx`, or `--format` is unknown. Nothing is written to stdout. |

---

## Example

`minimal.pptx` (one slide) against `three_slides.pptx` (three slides):

```json
{
  "a": "minimal.pptx",
  "b": "three_slides.pptx",
  "schema_version": 1,
  "slides": [
    {
      "a_index": 1,
      "b_index": 1,
      "changes": {
        "charts": [],
        "pictures": [],
        "shapes": [],
        "tables": [],
        "text": []
      },
      "moved": false,
      "similarity": 1.0,
      "status": "unchanged"
    },
    {
      "a_index": null,
      "b_index": 2,
      "changes": {
        "charts": [],
        "pictures": [],
        "shapes": [],
        "tables": [],
        "text": []
      },
      "moved": false,
      "similarity": 0.0,
      "status": "added"
    },
    {
      "a_index": null,
      "b_index": 3,
      "changes": {
        "charts": [],
        "pictures": [],
        "shapes": [],
        "tables": [],
        "text": []
      },
      "moved": false,
      "similarity": 0.0,
      "status": "added"
    }
  ],
  "summary": {
    "added": 2,
    "changed": 0,
    "moved": 0,
    "removed": 0,
    "unchanged": 1
  }
}
```

Keys are emitted in sorted order (as shown) because the serializer uses an
ordered map; consumers should not depend on key order.
