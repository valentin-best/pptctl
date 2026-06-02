# `pptctl extract` JSON Output Schema

**Current version: 1**

The `pptctl extract --format json` command emits a single JSON object to stdout.
This document is the authoritative spec for that output. Downstream tools (RAG
pipelines, indexers, test fixtures) should key off `schema_version` to guard
against breaking changes.

## Versioning policy

- `schema_version` is a plain integer at the root of every output document.
- **Breaking changes** (removing or renaming a field, changing a field's type
  or unit) bump `schema_version` by 1.
- **Additive changes** (new optional fields, new `kind` / chart-type values)
  do not bump the version; consumers should ignore unknown fields.

---

## Root object

| Field | Type | Description |
|---|---|---|
| `schema_version` | `integer` | Schema version. Currently `1`. |
| `file` | `string` | Path passed to the command. |
| `slide_count` | `integer` | Number of slides in the presentation. |
| `slides` | `array<Slide>` | One entry per slide, in presentation order. |

---

## Slide

| Field | Type | Description |
|---|---|---|
| `index` | `integer` | 1-based slide number. |
| `notes` | `string` | Speaker-notes text. Paragraph breaks are `\n`. Empty string when the slide has no notes. |
| `shapes` | `array<Shape>` | Top-level shapes on the slide, in document order. |

---

## Shape

| Field | Type | Description |
|---|---|---|
| `kind` | `string` | Shape type. One of: `placeholder`, `textbox`, `picture`, `table`, `chart`, `diagram`, `ole`, `group`, `connector`, `other`. |
| `id` | `string` | Shape id from `p:cNvPr@id`. |
| `name` | `string` | Shape name from `p:cNvPr@name`. |
| `placeholder_type` | `string \| null` | Placeholder type string (e.g. `"ctrTitle"`, `"body"`, `"title"`). `null` for non-placeholder shapes. |
| `geometry` | `Geometry \| null` | Position and size. `null` when the shape carries no `a:xfrm`. |
| `paragraphs` | `array<Paragraph>` | Text body. Empty array for shapes with no text (pictures, tables, charts). |
| `picture` | `Picture \| null` | Picture metadata. Non-null only when `kind == "picture"`. |
| `table` | `Table \| null` | Table data. Non-null only when `kind == "table"`. |
| `chart` | `Chart \| null` | Chart data. Non-null only when `kind == "chart"`. |
| `children` | `array<Shape>` | Child shapes. Non-empty only when `kind == "group"`. Recursive. |

Exactly one of `picture`, `table`, `chart` is non-null (or all are null for
text-only shapes). `paragraphs` and one of those three are mutually exclusive in
practice, but both arrays are always present.

---

## Geometry

Coordinates are in **EMU** (English Metric Units), the native OOXML unit.
1 inch = 914 400 EMU.

| Field | Type | Description |
|---|---|---|
| `off_x` | `integer` | X offset from the slide origin (left edge), in EMU. |
| `off_y` | `integer` | Y offset from the slide origin (top edge), in EMU. |
| `ext_cx` | `integer` | Width, in EMU. |
| `ext_cy` | `integer` | Height, in EMU. |

---

## Paragraph

| Field | Type | Description |
|---|---|---|
| `level` | `integer` | Indent / bullet level. `0` is the top level. |
| `text` | `string` | All runs concatenated into a single string. |

---

## Picture

| Field | Type | Description |
|---|---|---|
| `linked` | `boolean` | `true` when the image is an external link (`r:link`) rather than an embedded part (`r:embed`). |
| `target` | `string` | Package-internal path to the media part (e.g. `ppt/media/image1.png`). Empty string when `linked` is `true` or the relationship could not be resolved. |
| `content_type` | `string` | MIME type (e.g. `"image/png"`). Empty string when unknown. |
| `size_bytes` | `integer \| null` | Uncompressed size of the media part in bytes. `null` when unknown or unresolved. |
| `alt_text` | `string \| null` | Accessibility description from `p:cNvPr@descr`. `null` when absent. |

---

## Table

| Field | Type | Description |
|---|---|---|
| `columns` | `integer` | Number of columns (`a:tblGrid/a:gridCol` count). |
| `rows` | `array<TableRow>` | Rows in document order. |

### TableRow

| Field | Type | Description |
|---|---|---|
| `cells` | `array<TableCell>` | Cells in column order. The grid is always complete: merged cells are included as covered entries. |

### TableCell

| Field | Type | Description |
|---|---|---|
| `text` | `string` | Cell text. Multiple paragraphs are joined with `\n`. |
| `col_span` | `integer` | Number of columns this cell spans. `1` for a normal cell. |
| `row_span` | `integer` | Number of rows this cell spans. `1` for a normal cell. |
| `merged` | `boolean` | `true` for a covered (shadow) cell — the cell is occupied by a neighbor's span. Its `text` will be empty. |

---

## Chart

| Field | Type | Description |
|---|---|---|
| `type` | `string` | Chart type derived from the OOXML element name (e.g. `"bar"`, `"line"`, `"pie"`, `"bar3D"`). Combo charts use `+`-joined types (e.g. `"bar+line"`). Empty string when the chart part could not be resolved. |
| `title` | `string \| null` | Chart title text. `null` when absent. |
| `series` | `array<ChartSeries>` | Data series in document order. |

### ChartSeries

| Field | Type | Description |
|---|---|---|
| `name` | `string` | Series label (from `c:ser/c:tx`). |
| `categories` | `array<string>` | Category labels (from `c:ser/c:cat`). May be empty when the chart has no categories. |
| `values` | `array<number>` | Numeric data points (from `c:ser/c:val`). |

`categories` and `values` are parallel arrays when both are non-empty: index `i`
in `categories` corresponds to index `i` in `values`.
