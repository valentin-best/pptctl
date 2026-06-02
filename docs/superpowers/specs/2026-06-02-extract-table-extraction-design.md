# Design: extract — table extraction

- **Issue:** #9 (Phase 2 — extract, v0.2.0)
- **Branch:** `9-extract-table-extraction`
- **Date:** 2026-06-02
- **Builds on:** #7 (slide content parsing) and #8 (picture relationship resolution)

## Goal

Fill the `Table` slot left typed-but-shallow by #7. For each `p:graphicFrame`
whose `graphicData` is a table, parse the contained `a:tbl` into a grid of
rows and cells, each cell carrying full paragraphs/runs, with merged cells
represented as a span on the origin cell plus a covered flag on the cells it
hides.

This mirrors exactly how #8 filled the `Picture` slot: a struct attached only
to shapes of the relevant kind, populated during parsing. It extends the
parsing layer for the second non-text shape, before output formats (#12) and
golden tests (#14) lock anything down.

## Scope decisions (resolved during brainstorming)

1. **Merged cells: span on origin + mark covered.** The origin cell carries
   `col_span` / `row_span` (>1); cells it covers keep their grid position but
   are flagged `merged`. The grid stays rectangular so `rows[r].cells[c]`
   retains true coordinates. Fully satisfies the issue's merged-cell checkbox
   without losing grid geometry. (Rejected: dropping covered cells, which makes
   rows ragged; ignoring merges, which fails the checkbox.)
2. **Cell text: full paragraphs + runs.** Each cell reuses the existing
   `Paragraph`/`Run` model, so cell text keeps run-level formatting and bullet
   levels — consistent with text shapes and the `parse_text_body` helper.
   (Rejected: flattened per-cell strings, inconsistent with the rest of the
   model and discarding formatting the parser already extracts.)
3. **No relationship resolution.** Unlike pictures, tables are self-contained in
   the slide XML; there is nothing to resolve against the package. All work
   happens in `Slide::parse()`, none in `Slide::load()`.

## Data model (`include/pptctl/slide.hpp`)

New structs, attached only to `Table` shapes:

```cpp
// One cell of a table (a:tc). Reuses the text model for content. A cell that
// originates a merge carries col_span/row_span > 1; a cell covered by a
// neighbor's span is flagged merged and kept in the grid so row/column indices
// stay true.
struct TableCell {
    std::vector<Paragraph> paragraphs;  // a:tc/a:txBody
    int col_span = 1;                   // a:tc@gridSpan
    int row_span = 1;                   // a:tc@rowSpan
    bool merged = false;                // a:tc@hMerge or @vMerge (covered cell)

    [[nodiscard]] std::string text() const;  // paragraph texts joined with '\n'
};

struct TableRow {
    std::vector<TableCell> cells;  // a:tc, in document order
};

// A graphic-frame table (a:tbl). Populated only on shapes with kind == Table.
struct Table {
    std::size_t columns = 0;     // a:tblGrid/a:gridCol count
    std::vector<TableRow> rows;  // a:tr, in document order
};
```

`Shape` gains one field, alongside `picture`:

```cpp
std::optional<Table> table;  // populated only when kind == Table
```

`TableCell::text()` mirrors `Paragraph::text()` / the picture model's small
convenience accessors: it joins each paragraph's `text()` with `'\n'`.

## Parsing (`src/lib/slide.cpp`)

Replace the shallow `p:graphicFrame` branch in `parse_sp_tree` with a
`parse_graphic_frame(el)` helper:

- `set_identity(el, shape)` as today (id, name, geometry).
- `shape.kind = graphic_frame_kind(el)` as today.
- When `kind == ShapeKind::Table`, parse the table and assign
  `shape.table`.

Table parsing (`parse_table(gf_el)` → `Table`):

- Navigate `a:graphic/a:graphicData/a:tbl`.
- `columns` = number of `a:tblGrid/a:gridCol` children.
- For each `a:tr` → a `TableRow`; for each `a:tc` within it → a `TableCell`:
  - `col_span` = `@gridSpan` as int, default 1.
  - `row_span` = `@rowSpan` as int, default 1.
  - `merged` = true when `@hMerge` or `@vMerge` is present and truthy (these
    mark cells covered by a neighbor's horizontal/vertical span).
  - paragraphs filled from `a:tc/a:txBody` via the shared text-body helper.

### Refactor: share `parse_text_body`

`parse_text_body` currently appends paragraphs into a `Shape&`. Extract its body
into `parse_paragraphs(pugi::xml_node tx_body, std::vector<Paragraph>& out)` so
both shapes and table cells reuse it. The existing shape call site becomes a
thin wrapper (or calls the new helper with `shape.paragraphs`). No behavior
change for shapes; covered by the existing #7 tests.

pugixml is loaded with `parse_default`, so namespace prefixes are literal
(`a:tbl`, `a:tr`, `a:tc`, `gridSpan`, `hMerge`) — consistent with the rest of
the parser.

## `extract` command (`src/cmd/extract.cpp`)

Extend `print_shape` for `Table` shapes. Instead of the bare
`[table] "<name>"`, render a compact grid summary:

```
  [table] "Table 3" 2×2
    | Quarter | Revenue |
    | Q1      | 100     |
```

Rules:

- Header line: `[table] "<name>" <rows>×<columns>`.
- One line per row: cell `text()` values joined by ` | `, wrapped in leading and
  trailing `|`. Newlines within a cell collapse to a single space for the dump.
- A merged (covered) cell renders as empty; a cell with a span renders its text
  once in its origin position.

This is a small local helper. The dump is cosmetic and not a stable contract —
real output formats land in #12, golden tests in #14.

## Fixture (`tests/fixtures/generate_fixtures.py`)

Add `tables_deck.pptx`:

- Slide 1: a 2×2 table — a header row (`Quarter`, `Revenue`) and a data row
  (`Q1`, `100`), with the header cells bold so run formatting has coverage.
- Slide 2: a table with a horizontal merge (python-pptx `cell.merge()`), so the
  `gridSpan` / `hMerge` parsing path is exercised end-to-end.

Regenerate via the documented invocation:
`uv run --python 3.12 --with python-pptx tests/fixtures/generate_fixtures.py`.
No `CMakeLists.txt` change is needed: fixtures are reached as a directory via
the `PPTCTL_FIXTURES_DIR` macro, not enumerated individually.

## Testing (`tests/test_slide.cpp`)

- **parse-level, inline `a:tbl` XML:** `table` is set, `columns == 2`,
  `rows.size() == 2`, `rows[0].cells[0].text() == "Quarter"`, and run formatting
  is preserved on a bold header cell (`rows[0].cells[0].paragraphs[0].runs[0]
  .props.bold == true`).
- **parse-level, merged:** an inline `a:tbl` with a `gridSpan="2"` origin cell
  and an `hMerge="1"` covered cell — origin has `col_span == 2`, covered cell
  has `merged == true`, and the row still reports both cells.
- **load-level against `tables_deck.pptx`:** the table shape on slide 1 resolves
  with `columns == 2`, two rows, and expected cell text; slide 2's table has a
  cell with `col_span == 2`.
- **non-table graphicFrame** (e.g. a chart `graphicData` uri): `table` stays
  `nullopt` while `kind == Chart`.

No golden / byte-compared output files — that's #14.

## Out of scope (explicit)

- Chart data (#10), speaker notes (#11).
- Output formats and the `--format` flag (#12); versioned schema (#13); golden
  tests (#14).
- Cell fill, borders, alignment, column widths / row heights, and table styles
  (`a:tblPr` / `tableStyleId`).
- Resolving the table style part through the relationship graph.
- Mutating / serializing — this layer stays read-only.
