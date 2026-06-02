# Table Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parse `a:tbl` graphic-frame tables into a grid of rows/cells (with full paragraph/run text and merged-cell spans) on the slide content model, and surface them in the `extract` dump.

**Architecture:** Mirror how #8 filled the `Picture` slot. A `Table` struct attached only to `Table` shapes, populated entirely in `Slide::parse()` (tables are self-contained — no relationship resolution, unlike pictures). Cells reuse the existing `Paragraph`/`Run` text model via a shared `parse_paragraphs` helper. Merged cells keep their grid position with a `merged` flag; the origin cell carries `col_span`/`row_span`.

**Tech Stack:** C++20, pugixml (parsing), doctest (tests), python-pptx via `uv` (fixture generation), CMake/Ninja.

---

## File Structure

- **Modify** `include/pptctl/slide.hpp` — add `TableCell`, `TableRow`, `Table` structs; add `std::optional<Table> table` to `Shape`.
- **Modify** `src/lib/slide.cpp` — refactor `parse_text_body` into a reusable `parse_paragraphs`; add `parse_table` + `parse_graphic_frame`; implement `TableCell::text()`.
- **Modify** `src/cmd/extract.cpp` — render tables in `print_shape`.
- **Modify** `tests/test_slide.cpp` — parse-level and load-level table tests; extend the existing mixed-shapes test.
- **Modify** `tests/fixtures/generate_fixtures.py` — add `build_tables_deck`; wire into `main()`.
- **Create** `tests/fixtures/tables_deck.pptx` — generated binary fixture (2 slides: plain 2×2 table, merged-header table).

Build/run commands used throughout:
- Build + test: `./scripts/build.sh release`
- Single test case: `./build/macos-release/pptctl_tests --test-case="<name>"`
- Format (pre-commit hook also enforces this): `clang-format -i <files>`

---

## Task 1: Refactor `parse_text_body` into a reusable `parse_paragraphs`

Tables cells contain an `a:txBody` exactly like text shapes. Extract the paragraph-parsing body so cells and shapes share it. Pure refactor — no behavior change, existing #7 tests stay green.

**Files:**
- Modify: `src/lib/slide.cpp:43-68` (the `parse_text_body` function) and its call site at `src/lib/slide.cpp:130-132` (in `parse_sp`).

- [ ] **Step 1: Replace `parse_text_body` with `parse_paragraphs`**

In `src/lib/slide.cpp`, replace the whole `parse_text_body` function (lines 43–68) with a version that fills a vector instead of a `Shape`:

```cpp
void parse_paragraphs(pugi::xml_node tx_body, std::vector<Paragraph>& out) {
    for (auto p : tx_body.children("a:p")) {
        Paragraph para;
        para.level = p.child("a:pPr").attribute("lvl").as_int(0);
        for (auto node : p.children()) {
            const std::string_view name = node.name();
            if (name == "a:r") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                run.props = parse_run_props(node.child("a:rPr"));
                para.runs.push_back(std::move(run));
            } else if (name == "a:fld") {
                Run run;
                run.text = node.child("a:t").text().as_string("");
                run.props = parse_run_props(node.child("a:rPr"));
                run.is_field = true;
                para.runs.push_back(std::move(run));
            } else if (name == "a:br") {
                Run run;
                run.text = "\n";
                para.runs.push_back(std::move(run));
            }
        }
        out.push_back(std::move(para));
    }
}
```

- [ ] **Step 2: Update the `parse_sp` call site**

In `parse_sp` (around `src/lib/slide.cpp:130`), change:

```cpp
    if (auto tx = sp_el.child("p:txBody")) {
        parse_text_body(tx, shape);
    }
```

to:

```cpp
    if (auto tx = sp_el.child("p:txBody")) {
        parse_paragraphs(tx, shape.paragraphs);
    }
```

- [ ] **Step 3: Build and run the existing slide tests**

Run: `./scripts/build.sh release`
Expected: builds clean; the `unit` test passes. The existing text/paragraph tests (`parses a single text shape...`, `parses run properties...`, `parses levels, empty paragraphs...`) still pass — this is a behavior-preserving refactor.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/lib/slide.cpp
git add src/lib/slide.cpp
git commit -m "refactor(extract): share paragraph parsing across shapes and cells (#9)"
```

---

## Task 2: Add the table data model + `TableCell::text()`

**Files:**
- Modify: `include/pptctl/slide.hpp` (add structs after `Paragraph`/before `Geometry`, and a field on `Shape`).
- Modify: `src/lib/slide.cpp` (implement `TableCell::text()`).
- Test: `tests/test_slide.cpp` (unit test for `text()`).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_slide.cpp`, after the last test case (after line 469):

```cpp
TEST_CASE("TableCell: text joins paragraph texts with newlines") {
    pptctl::TableCell cell;
    pptctl::Paragraph p1;
    p1.runs.push_back(pptctl::Run{.text = "Line A"});
    pptctl::Paragraph p2;
    p2.runs.push_back(pptctl::Run{.text = "Line B"});
    cell.paragraphs = {p1, p2};
    CHECK(cell.text() == "Line A\nLine B");

    pptctl::TableCell empty;
    CHECK(empty.text().empty());
}
```

- [ ] **Step 2: Run the test to verify it fails (does not compile)**

Run: `./scripts/build.sh release`
Expected: FAIL — compile error, `TableCell` is not a member of `pptctl`.

- [ ] **Step 3: Add the data model to `slide.hpp`**

In `include/pptctl/slide.hpp`, insert these structs immediately after the `Paragraph` struct closes (after line 66, before the `Geometry` comment on line 68):

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

Then add a field to `Shape`, immediately after the `picture` field (after line 98):

```cpp
    std::optional<Table> table;  // populated only when kind == Table
```

- [ ] **Step 4: Implement `TableCell::text()` in `slide.cpp`**

In `src/lib/slide.cpp`, add the definition just after the `namespace pptctl {` opening and before the anonymous `namespace {` (i.e. as a free-standing member definition near the top, after line 7):

```cpp
std::string TableCell::text() const {
    std::string out;
    for (const auto& p : paragraphs) {
        if (!out.empty()) {
            out += '\n';
        }
        out += p.text();
    }
    return out;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl_tests --test-case="TableCell: text joins paragraph texts with newlines"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
clang-format -i include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp
git add include/pptctl/slide.hpp src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): add table content model to the slide tree (#9)"
```

---

## Task 3: Parse basic tables (rows, columns, cell text)

**Files:**
- Modify: `src/lib/slide.cpp` (add `parse_table` + `parse_graphic_frame`; wire into `parse_sp_tree`).
- Test: `tests/test_slide.cpp` (new fixture XML + parse test; extend mixed-shapes test).

- [ ] **Step 1: Write the failing test**

In `tests/test_slide.cpp`, add this XML constant inside the anonymous namespace, after `kGroupedPicture` (after line 213):

```cpp
// A 2x2 table inside a graphicFrame: a bold header row and a data row.
constexpr const char* kBasicTable = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="2" name="Table 2"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table">
        <a:tbl>
          <a:tblGrid><a:gridCol w="100"/><a:gridCol w="100"/></a:tblGrid>
          <a:tr h="100">
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:rPr b="1"/><a:t>Quarter</a:t></a:r></a:p></a:txBody></a:tc>
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>Revenue</a:t></a:r></a:p></a:txBody></a:tc>
          </a:tr>
          <a:tr h="100">
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>Q1</a:t></a:r></a:p></a:txBody></a:tc>
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>100</a:t></a:r></a:p></a:txBody></a:tc>
          </a:tr>
        </a:tbl>
      </a:graphicData></a:graphic>
    </p:graphicFrame>
  </p:spTree></p:cSld>
</p:sld>)";
```

Then add this test case after the `TableCell` test from Task 2:

```cpp
TEST_CASE("Slide: parses a table's rows, columns, and cell text") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kBasicTable));
    REQUIRE(slide.shapes().size() == 1);

    const auto& shape = slide.shapes()[0];
    CHECK(shape.kind == pptctl::ShapeKind::Table);
    CHECK(shape.name == "Table 2");
    REQUIRE(shape.table.has_value());

    const auto& table = *shape.table;
    CHECK(table.columns == 2);
    REQUIRE(table.rows.size() == 2);
    REQUIRE(table.rows[0].cells.size() == 2);
    REQUIRE(table.rows[1].cells.size() == 2);

    CHECK(table.rows[0].cells[0].text() == "Quarter");
    CHECK(table.rows[0].cells[1].text() == "Revenue");
    CHECK(table.rows[1].cells[0].text() == "Q1");
    CHECK(table.rows[1].cells[1].text() == "100");

    // Cell text keeps run formatting (bold header).
    const auto& header = table.rows[0].cells[0].paragraphs.at(0).runs.at(0);
    REQUIRE(header.props.bold.has_value());
    CHECK(*header.props.bold == true);
}
```

Also extend the existing mixed-shapes test. In `TEST_CASE("Slide: detects shape kinds, placeholder type, and geometry")`, after the line `CHECK(s[3].kind == pptctl::ShapeKind::Chart);` (line 322), add:

```cpp
    // A chart graphicFrame (no a:tbl) carries no table model.
    CHECK_FALSE(s[3].table.has_value());
    // The table graphicFrame in this fixture has no a:tbl child either, so no
    // table model is attached even though its kind is Table.
    CHECK_FALSE(s[2].table.has_value());
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./scripts/build.sh release`
Expected: FAIL — `shape.table` has no value (the `p:graphicFrame` branch in `parse_sp_tree` never parses the table).

- [ ] **Step 3: Add `parse_table` and `parse_graphic_frame`**

In `src/lib/slide.cpp`, add these two functions in the anonymous namespace, right after `graphic_frame_kind` (after line 116):

```cpp
Table parse_table(pugi::xml_node tbl) {
    Table table;
    std::size_t cols = 0;
    for (auto gc : tbl.child("a:tblGrid").children("a:gridCol")) {
        (void)gc;
        ++cols;
    }
    table.columns = cols;
    for (auto tr : tbl.children("a:tr")) {
        TableRow row;
        for (auto tc : tr.children("a:tc")) {
            TableCell cell;
            parse_paragraphs(tc.child("a:txBody"), cell.paragraphs);
            row.cells.push_back(std::move(cell));
        }
        table.rows.push_back(std::move(row));
    }
    return table;
}

Shape parse_graphic_frame(pugi::xml_node gf_el) {
    Shape shape;
    shape.kind = graphic_frame_kind(gf_el);
    set_identity(gf_el, shape);
    if (shape.kind == ShapeKind::Table) {
        auto tbl = gf_el.child("a:graphic").child("a:graphicData").child("a:tbl");
        if (tbl) {
            shape.table = parse_table(tbl);
        }
    }
    return shape;
}
```

- [ ] **Step 4: Wire it into `parse_sp_tree`**

In `parse_sp_tree`, replace the existing `p:graphicFrame` branch:

```cpp
        } else if (name == "p:graphicFrame") {
            Shape shape;
            shape.kind = graphic_frame_kind(el);
            set_identity(el, shape);
            out.push_back(std::move(shape));
        } else if (name == "p:cxnSp") {
```

with:

```cpp
        } else if (name == "p:graphicFrame") {
            out.push_back(parse_graphic_frame(el));
        } else if (name == "p:cxnSp") {
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl_tests --test-case="Slide: parses a table*"`
and `./build/macos-release/pptctl_tests --test-case="Slide: detects shape kinds*"`
Expected: both PASS. The full `unit` suite also passes.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): parse table rows, columns, and cell text (#9)"
```

---

## Task 4: Parse merged cells (spans + covered flag)

**Files:**
- Modify: `src/lib/slide.cpp` (read `gridSpan`/`rowSpan`/`hMerge`/`vMerge` in `parse_table`).
- Test: `tests/test_slide.cpp` (new merged-table fixture XML + test).

- [ ] **Step 1: Write the failing test**

In `tests/test_slide.cpp`, add this XML constant after `kBasicTable` in the anonymous namespace:

```cpp
// A 2x2 table whose top row is merged across both columns: the origin cell has
// gridSpan="2" and the covered cell carries hMerge="1".
constexpr const char* kMergedTable = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="2" name="Merged 2"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table">
        <a:tbl>
          <a:tblGrid><a:gridCol w="100"/><a:gridCol w="100"/></a:tblGrid>
          <a:tr h="100">
            <a:tc gridSpan="2"><a:txBody><a:bodyPr/><a:p><a:r><a:t>Annual Summary</a:t></a:r></a:p></a:txBody></a:tc>
            <a:tc hMerge="1"><a:txBody><a:bodyPr/><a:p/></a:txBody></a:tc>
          </a:tr>
          <a:tr h="100">
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>Left</a:t></a:r></a:p></a:txBody></a:tc>
            <a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>Right</a:t></a:r></a:p></a:txBody></a:tc>
          </a:tr>
        </a:tbl>
      </a:graphicData></a:graphic>
    </p:graphicFrame>
  </p:spTree></p:cSld>
</p:sld>)";
```

Then add this test case after the basic-table test:

```cpp
TEST_CASE("Slide: captures merged cells as span on origin + covered flag") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kMergedTable));
    REQUIRE(slide.shapes().size() == 1);
    REQUIRE(slide.shapes()[0].table.has_value());

    const auto& table = *slide.shapes()[0].table;
    REQUIRE(table.rows.size() == 2);
    // The grid stays rectangular: the covered cell is kept, not dropped.
    REQUIRE(table.rows[0].cells.size() == 2);

    const auto& origin = table.rows[0].cells[0];
    CHECK(origin.col_span == 2);
    CHECK(origin.row_span == 1);
    CHECK_FALSE(origin.merged);
    CHECK(origin.text() == "Annual Summary");

    const auto& covered = table.rows[0].cells[1];
    CHECK(covered.merged);

    // A plain cell keeps the defaults.
    const auto& plain = table.rows[1].cells[0];
    CHECK(plain.col_span == 1);
    CHECK(plain.row_span == 1);
    CHECK_FALSE(plain.merged);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl_tests --test-case="Slide: captures merged cells*"`
Expected: FAIL — `origin.col_span` is 1 (parser doesn't read `gridSpan` yet) and `covered.merged` is false.

- [ ] **Step 3: Read the span/merge attributes in `parse_table`**

In `src/lib/slide.cpp`, in `parse_table`, expand the cell-building block. Replace:

```cpp
            TableCell cell;
            parse_paragraphs(tc.child("a:txBody"), cell.paragraphs);
            row.cells.push_back(std::move(cell));
```

with:

```cpp
            TableCell cell;
            cell.col_span = tc.attribute("gridSpan").as_int(1);
            cell.row_span = tc.attribute("rowSpan").as_int(1);
            cell.merged =
                tc.attribute("hMerge").as_bool() || tc.attribute("vMerge").as_bool();
            parse_paragraphs(tc.child("a:txBody"), cell.paragraphs);
            row.cells.push_back(std::move(cell));
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl_tests --test-case="Slide: captures merged cells*"`
Expected: PASS. Full `unit` suite still passes.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/lib/slide.cpp tests/test_slide.cpp
git add src/lib/slide.cpp tests/test_slide.cpp
git commit -m "feat(extract): capture merged table cells (span + covered flag) (#9)"
```

---

## Task 5: Add the `tables_deck.pptx` fixture + load-level test

**Files:**
- Modify: `tests/fixtures/generate_fixtures.py` (add `build_tables_deck`, wire into `main`).
- Create: `tests/fixtures/tables_deck.pptx` (generated binary).
- Test: `tests/test_slide.cpp` (load-level test against the fixture).

- [ ] **Step 1: Add the fixture builder**

In `tests/fixtures/generate_fixtures.py`, add this function after `build_notes_deck_base` (after line 101, before `_next_index`):

```python
def build_tables_deck(path: Path) -> None:
    """2 slides: a plain 2x2 table (bold header), and a table whose top row is
    merged across both columns."""
    prs = Presentation()
    blank = prs.slide_layouts[6]

    # Slide 1: a 2x2 table with a bold header row.
    s1 = prs.slides.add_slide(blank)
    t1 = s1.shapes.add_table(
        2, 2, Inches(1), Inches(1), Inches(6), Inches(2)
    ).table
    for col, header in enumerate(("Quarter", "Revenue")):
        cell = t1.cell(0, col)
        cell.text = header
        cell.text_frame.paragraphs[0].runs[0].font.bold = True
    t1.cell(1, 0).text = "Q1"
    t1.cell(1, 1).text = "100"

    # Slide 2: a 2x2 table with the top row merged across both columns.
    s2 = prs.slides.add_slide(blank)
    t2 = s2.shapes.add_table(
        2, 2, Inches(1), Inches(1), Inches(6), Inches(2)
    ).table
    t2.cell(0, 0).merge(t2.cell(0, 1))
    t2.cell(0, 0).text = "Annual Summary"
    t2.cell(1, 0).text = "Left"
    t2.cell(1, 1).text = "Right"

    _set_core_props(prs, "pptctl tables fixture")
    prs.save(str(path))
```

- [ ] **Step 2: Wire it into `main()`**

In `main()` (around line 199), add the fixture path and build call, and update the print line:

```python
def main() -> None:
    images = HERE / "images_deck.pptx"
    notes = HERE / "notes_and_masters.pptx"
    tables = HERE / "tables_deck.pptx"
    build_images_deck(images)
    build_notes_deck_base(notes)
    inject_second_master(notes)
    build_tables_deck(tables)
    print(f"wrote {images.name}, {notes.name}, and {tables.name} to {HERE}")
```

- [ ] **Step 3: Regenerate the fixtures**

Run: `uv run --python 3.12 --with python-pptx tests/fixtures/generate_fixtures.py`
Expected: prints `wrote images_deck.pptx, notes_and_masters.pptx, and tables_deck.pptx to ...`; `tests/fixtures/tables_deck.pptx` now exists.

- [ ] **Step 4: Sanity-check the fixture with the parser**

Run: `./build/macos-release/pptctl extract tests/fixtures/tables_deck.pptx`
Expected: two slides; each shows a `[table]` line. (The exact format lands in Task 6 — at this point the table prints with the generic `[table] "<name>"` line, which is fine.)

- [ ] **Step 5: Write the load-level test**

In `tests/test_slide.cpp`, add after the merged-cells test:

```cpp
TEST_CASE("Slide: loads tables from tables_deck.pptx") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("tables_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    REQUIRE(pres.slide_parts().size() == 2);

    auto first_table = [](const pptctl::Slide& slide) -> const pptctl::Table* {
        for (const auto& s : slide.shapes()) {
            if (s.kind == pptctl::ShapeKind::Table && s.table.has_value()) {
                return &*s.table;
            }
        }
        return nullptr;
    };

    // Slide 1: a plain 2x2 table.
    pptctl::Slide s1;
    REQUIRE(s1.load(pkg, pres.slide_parts().at(0)));
    const pptctl::Table* t1 = first_table(s1);
    REQUIRE(t1 != nullptr);
    CHECK(t1->columns == 2);
    REQUIRE(t1->rows.size() == 2);
    REQUIRE(t1->rows[0].cells.size() == 2);
    CHECK(t1->rows[0].cells[0].text() == "Quarter");
    CHECK(t1->rows[0].cells[1].text() == "Revenue");
    CHECK(t1->rows[1].cells[0].text() == "Q1");

    // Slide 2: the top row is merged across both columns.
    pptctl::Slide s2;
    REQUIRE(s2.load(pkg, pres.slide_parts().at(1)));
    const pptctl::Table* t2 = first_table(s2);
    REQUIRE(t2 != nullptr);
    REQUIRE(t2->rows.size() == 2);
    REQUIRE(t2->rows[0].cells.size() == 2);
    CHECK(t2->rows[0].cells[0].col_span == 2);
    CHECK(t2->rows[0].cells[0].text() == "Annual Summary");
    CHECK(t2->rows[0].cells[1].merged);
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl_tests --test-case="Slide: loads tables*"`
Expected: PASS. If `col_span`/`merged` assertions fail, inspect the generated XML with
`unzip -p tests/fixtures/tables_deck.pptx ppt/slides/slide2.xml` to confirm python-pptx emitted `gridSpan`/`hMerge` (it does for `cell.merge`).

- [ ] **Step 7: Commit (including the binary fixture)**

```bash
git add tests/fixtures/generate_fixtures.py tests/fixtures/tables_deck.pptx tests/test_slide.cpp
git commit -m "test(extract): add tables_deck fixture and load-level table test (#9)"
```

---

## Task 6: Render tables in the `extract` dump

**Files:**
- Modify: `src/cmd/extract.cpp` (add a table-rendering helper + a branch in `print_shape`).

This output is cosmetic (the stable formats are #12; golden tests are #14), so it is verified by running the binary rather than a unit test — consistent with how the picture dump was verified.

- [ ] **Step 1: Add a `print_table` helper**

In `src/cmd/extract.cpp`, inside the anonymous namespace, add this helper just before `print_shape` (before line 86). It needs `<pptctl/slide.hpp>` types, already included.

```cpp
// One-line-per-row grid summary for a table shape. Cosmetic — the stable
// output formats are tracked in #12.
void print_table(const Table& table, const std::string& pad, std::string_view name) {
    pptctl::println("{}[table] \"{}\" {}x{}", pad, name, table.rows.size(), table.columns);
    for (const auto& row : table.rows) {
        std::string line = "|";
        for (const auto& cell : row.cells) {
            std::string text = cell.text();
            for (auto& ch : text) {
                if (ch == '\n') {
                    ch = ' ';
                }
            }
            line += std::format(" {} |", text);
        }
        pptctl::println("{}  {}", pad, line);
    }
}
```

- [ ] **Step 2: Add the branch in `print_shape`**

In `print_shape`, add a `table` branch before the generic `else`. Change:

```cpp
    } else if (shape.picture) {
        pptctl::println("{}[picture] \"{}\" {}", pad, shape.name, describe_picture(*shape.picture));
    } else {
        pptctl::println("{}[{}] \"{}\"", pad, kind_label(shape.kind), shape.name);
    }
```

to:

```cpp
    } else if (shape.picture) {
        pptctl::println("{}[picture] \"{}\" {}", pad, shape.name, describe_picture(*shape.picture));
    } else if (shape.table) {
        print_table(*shape.table, pad, shape.name);
    } else {
        pptctl::println("{}[{}] \"{}\"", pad, kind_label(shape.kind), shape.name);
    }
```

- [ ] **Step 3: Build and verify the output by running the binary**

Run: `./scripts/build.sh release` then
`./build/macos-release/pptctl extract tests/fixtures/tables_deck.pptx`
Expected output (shape names assigned by python-pptx may differ slightly):

```
slide 1
  [table] "Table 1" 2x2
    | Quarter | Revenue |
    | Q1 | 100 |
slide 2
  [table] "Table 1" 2x2
    | Annual Summary |  |
    | Left | Right |
```

The merged top row shows its text once with an empty covered cell.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/cmd/extract.cpp
git add src/cmd/extract.cpp
git commit -m "feat(extract): render tables in the extract dump (#9)"
```

---

## Task 7: Full verification + open the PR

**Files:** none (verification + integration).

- [ ] **Step 1: Run the full suite under the asan preset**

Run: `./scripts/build.sh asan`
Expected: configures, builds clean (strict warnings, no new warnings in project code), `unit` test passes with AddressSanitizer/UBSan clean. This catches any out-of-bounds or UB the release build masks.

- [ ] **Step 2: Confirm the whole release suite is green**

Run: `./scripts/build.sh release`
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 3: Verify nothing is uncommitted**

Run: `git status -s`
Expected: empty working tree (all changes committed across Tasks 1–6).

- [ ] **Step 4: Push and open the PR**

```bash
git push -u origin 9-extract-table-extraction
gh pr create --base main \
  --title "extract: table extraction (#9)" \
  --body "Closes #9.

Parses a:tbl graphic-frame tables into rows/cells with full paragraph/run text, and handles merged cells (span on origin + covered flag). Mirrors the #8 Picture pattern; tables are self-contained so all work happens in Slide::parse().

## Acceptance criteria
- [x] Rows, columns, cell text
- [x] Merged-cell handling (gridSpan/rowSpan + hMerge/vMerge)

## Testing
- Parse-level: basic 2x2 table (cell text + bold-run formatting preserved), merged-header table (col_span on origin, merged flag on covered cell)
- Load-level: new tables_deck.pptx fixture (plain table + merged table)
- Extended the mixed-shapes test: chart/empty graphicFrame carries no table model
- Verified the extract dump on tables_deck.pptx

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

Expected: PR URL printed; the issue will auto-close on merge.

---

## Self-Review Notes

- **Spec coverage:** rows/columns/cell text → Tasks 3, 5; merged-cell handling → Task 4; full paragraphs+runs per cell → Tasks 1, 3 (bold assertion); data model → Task 2; parser refactor seam → Task 1; extract dump → Task 6; fixture → Task 5; non-table guard → Task 3 (extended mixed-shapes test). All spec sections map to a task.
- **Type consistency:** `Table.columns` (`std::size_t`), `Table.rows`, `TableRow.cells`, `TableCell.{paragraphs,col_span,row_span,merged,text()}`, and `Shape.table` are used identically in `slide.hpp`, `slide.cpp`, `extract.cpp`, and the tests.
- **No placeholders:** every code/command step is concrete.
