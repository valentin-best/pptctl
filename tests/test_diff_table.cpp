// Tests for the table-level diff (Phase 4, #74). Owned by the table/chart diff
// issue; keep table-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <ostream>
#include <pptctl/diff.hpp>
#include <pptctl/slide.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace pptctl::cmd {
void render(const DiffReport& report,
            const std::string& a,
            const std::string& b,
            std::ostream& os,
            const DiffOutputOptions& out);
}  // namespace pptctl::cmd

namespace {

using pptctl::TableChange;
using pptctl::TableOp;

// One a:tc whose body is `paras` (concatenated a:p elements); `attrs` lands on
// the a:tc (gridSpan="2", hMerge="1", ...).
std::string cell_raw(const std::string& paras, const std::string& attrs = "") {
    return "<a:tc " + attrs + "><a:txBody><a:bodyPr/>" + paras + "</a:txBody></a:tc>";
}

// One a:tc with a single plain-run paragraph.
std::string cell(const std::string& text, const std::string& attrs = "") {
    return cell_raw("<a:p><a:r><a:t>" + text + "</a:t></a:r></a:p>", attrs);
}

std::string row(const std::vector<std::string>& cells) {
    std::string out = "<a:tr h=\"100\">";
    for (const auto& c : cells) {
        out += c;
    }
    return out + "</a:tr>";
}

// A row of plain cells.
std::string prow(const std::vector<std::string>& texts) {
    std::vector<std::string> cells;
    cells.reserve(texts.size());
    for (const auto& t : texts) {
        cells.push_back(cell(t));
    }
    return row(cells);
}

// A slide with one table graphic frame of `cols` grid columns and the given
// rows (concatenated a:tr elements).
std::string table_slide(std::size_t cols,
                        const std::string& rows,
                        const char* id = "2",
                        const char* name = "Table 1") {
    std::string grid;
    for (std::size_t i = 0; i < cols; ++i) {
        grid += "<a:gridCol w=\"100\"/>";
    }
    return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id=")") +
           id + R"(" name=")" + name + R"("/></p:nvGraphicFramePr>
      <p:xfrm><a:off x="100" y="200"/><a:ext cx="300" cy="400"/></p:xfrm>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table">
        <a:tbl><a:tblGrid>)" +
           grid + "</a:tblGrid>" + rows + R"(</a:tbl>
      </a:graphicData></a:graphic>
    </p:graphicFrame>
  </p:spTree></p:cSld>
</p:sld>)";
}

pptctl::Slide parse(const std::string& xml) {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    return s;
}

std::vector<TableChange> tables_between(const std::string& a_xml,
                                        const std::string& b_xml,
                                        const pptctl::DiffOptions& opts = {}) {
    const auto a = parse(a_xml);
    const auto b = parse(b_xml);
    return pptctl::compare_slides(a, b, opts).tables;
}

// The 2x2 table of tables_deck.pptx slide 1: a header row and one data row.
const std::string kBase = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"});

}  // namespace

// ── comparison ────────────────────────────────────────────────────────────────

TEST_CASE("Diff/table: identical tables yield no table ops") {
    CHECK(tables_between(table_slide(2, kBase), table_slide(2, kBase)).empty());
}

TEST_CASE("Diff/table: one edited cell is one cell_changed op with its coordinates") {
    const auto ops = tables_between(
        table_slide(2, kBase), table_slide(2, prow({"Quarter", "Revenue"}) + prow({"Q1", "120"})));
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.op == TableOp::CellChanged);
    CHECK(c.shape_id == "2");
    CHECK(c.shape_name == "Table 1");
    CHECK(c.a_row == 2);
    CHECK(c.b_row == 2);
    CHECK(c.col == 2);
    CHECK(c.old_text == "100");
    CHECK(c.new_text == "120");
}

TEST_CASE("Diff/table: an inserted row is one rows_added op and shifts nothing into changed") {
    const std::string a = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) + prow({"Q2", "200"});
    const std::string b = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) +
                          prow({"Q1b", "150"}) + prow({"Q2", "200"});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::RowsAdded);
    CHECK(ops[0].indices == std::vector<std::size_t>{3});
}

TEST_CASE("Diff/table: a removed row is one rows_removed op naming the A row") {
    const std::string a = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) + prow({"Q2", "200"}) +
                          prow({"Q3", "300"});
    const std::string b = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) + prow({"Q3", "300"});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::RowsRemoved);
    CHECK(ops[0].indices == std::vector<std::size_t>{3});
}

TEST_CASE("Diff/table: a row inserted above an edited row keeps the edit a cell change") {
    const std::string a = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) + prow({"Q2", "200"});
    const std::string b = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"}) +
                          prow({"Q1b", "150"}) + prow({"Q2", "250"});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == TableOp::RowsAdded);
    CHECK(ops[0].indices == std::vector<std::size_t>{3});
    CHECK(ops[1].op == TableOp::CellChanged);
    CHECK(ops[1].a_row == 3);
    CHECK(ops[1].b_row == 4);
    CHECK(ops[1].col == 2);
    CHECK(ops[1].old_text == "200");
    CHECK(ops[1].new_text == "250");
}

TEST_CASE("Diff/table: a fully rewritten row in a same-size gap is cell changes, not add+remove") {
    const std::string a = prow({"Quarter", "Revenue"}) + prow({"Q1", "100"});
    const std::string b = prow({"Quarter", "Revenue"}) + prow({"Q9", "999"});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == TableOp::CellChanged);
    CHECK(ops[0].col == 1);
    CHECK(ops[1].op == TableOp::CellChanged);
    CHECK(ops[1].col == 2);
}

TEST_CASE("Diff/table: an added column is one columns_added op; the common prefix compares") {
    const std::string b = prow({"Quarter", "Revenue", "Cost"}) + prow({"Q1", "100", "80"});
    const auto ops = tables_between(table_slide(2, kBase), table_slide(3, b));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::ColumnsAdded);
    CHECK(ops[0].indices == std::vector<std::size_t>{3});
}

TEST_CASE("Diff/table: a removed column is one columns_removed op naming the A columns") {
    const std::string b = prow({"Quarter"}) + prow({"Q1"});
    const auto ops = tables_between(table_slide(2, kBase), table_slide(1, b));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::ColumnsRemoved);
    CHECK(ops[0].indices == std::vector<std::size_t>{2});
}

TEST_CASE("Diff/table: merging the header row is a merge_changed op per affected cell") {
    const std::string a = row({cell("Annual Summary"), cell("")}) + prow({"Left", "Right"});
    const std::string b =
        row({cell("Annual Summary", "gridSpan=\"2\""), cell("", "hMerge=\"1\"")}) +
        prow({"Left", "Right"});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == TableOp::MergeChanged);
    CHECK(ops[0].a_row == 1);
    CHECK(ops[0].b_row == 1);
    CHECK(ops[0].col == 1);
    REQUIRE(ops[0].old_merge);
    REQUIRE(ops[0].new_merge);
    CHECK(ops[0].old_merge->col_span == 1);
    CHECK(ops[0].new_merge->col_span == 2);
    CHECK_FALSE(ops[0].new_merge->covered);
    CHECK(ops[1].op == TableOp::MergeChanged);
    CHECK(ops[1].col == 2);
    CHECK(ops[1].new_merge->covered);
}

TEST_CASE("Diff/table: whitespace-only cell differences are ignored unless strict") {
    const std::string b = prow({"Quarter", "Revenue"}) + row({cell("Q1"), cell("  100 ")});
    CHECK(tables_between(table_slide(2, kBase), table_slide(2, b)).empty());

    pptctl::DiffOptions strict;
    strict.strict_text = true;
    const auto ops = tables_between(table_slide(2, kBase), table_slide(2, b), strict);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::CellChanged);
    CHECK(ops[0].new_text == "  100 ");
}

TEST_CASE("Diff/table: field runs in a cell are ignored unless strict") {
    const std::string fld =
        "<a:p><a:r><a:t>Page </a:t></a:r><a:fld id=\"{1}\" type=\"slidenum\">"
        "<a:t>3</a:t></a:fld></a:p>";
    const std::string fld2 =
        "<a:p><a:r><a:t>Page </a:t></a:r><a:fld id=\"{1}\" type=\"slidenum\">"
        "<a:t>4</a:t></a:fld></a:p>";
    const std::string a = prow({"Quarter", "Revenue"}) + row({cell("Q1"), cell_raw(fld)});
    const std::string b = prow({"Quarter", "Revenue"}) + row({cell("Q1"), cell_raw(fld2)});
    CHECK(tables_between(table_slide(2, a), table_slide(2, b)).empty());

    pptctl::DiffOptions strict;
    strict.strict_text = true;
    CHECK(tables_between(table_slide(2, a), table_slide(2, b), strict).size() == 1);
}

TEST_CASE("Diff/table: multi-paragraph cells compare at paragraph-join level") {
    const std::string two =
        "<a:p><a:r><a:t>one</a:t></a:r></a:p><a:p><a:r><a:t>two</a:t></a:r></a:p>";
    const std::string a = prow({"Quarter", "Revenue"}) + row({cell("Q1"), cell_raw(two)});
    const std::string b = prow({"Quarter", "Revenue"}) + row({cell("Q1"), cell("one")});
    const auto ops = tables_between(table_slide(2, a), table_slide(2, b));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == TableOp::CellChanged);
    CHECK(ops[0].old_text == "one\ntwo");
    CHECK(ops[0].new_text == "one");
}

TEST_CASE("Diff/table: tables are matched by name when ids differ; ops name the B shape") {
    const auto ops = tables_between(
        table_slide(2, kBase, "2", "Table 1"),
        table_slide(2, prow({"Quarter", "Revenue"}) + prow({"Q1", "120"}), "7", "Table 1"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].shape_id == "7");
    CHECK(ops[0].shape_name == "Table 1");
}

TEST_CASE("Diff/table: an unmatched table yields no table ops (the shape diff reports it)") {
    const auto changes = pptctl::compare_slides(
        parse(table_slide(2, kBase, "2", "Table 1")),
        parse(table_slide(2, prow({"X", "Y"}) + prow({"1", "2"}), "9", "Other")),
        {});
    CHECK(changes.tables.empty());
    CHECK(changes.shapes.size() == 2);
}

// ── describe / to_json ────────────────────────────────────────────────────────

TEST_CASE("Diff/table: describe() gives one readable line per op") {
    TableChange c;
    c.shape_name = "Table 1";
    c.op = TableOp::CellChanged;
    c.a_row = 2;
    c.b_row = 2;
    c.col = 2;
    c.old_text = "100";
    c.new_text = "120";
    CHECK(describe(c) == R"(table "Table 1": cell (2,2) changed: "100" -> "120")");

    c.b_row = 3;
    CHECK(describe(c) == R"(table "Table 1": cell (2,2) (now (3,2)) changed: "100" -> "120")");

    TableChange r;
    r.shape_name = "Table 1";
    r.op = TableOp::RowsAdded;
    r.indices = {3};
    CHECK(describe(r) == R"(table "Table 1": 1 row added (row 3))");
    r.op = TableOp::RowsRemoved;
    r.indices = {2, 3};
    CHECK(describe(r) == R"(table "Table 1": 2 rows removed (rows 2, 3))");
    r.op = TableOp::ColumnsAdded;
    r.indices = {3};
    CHECK(describe(r) == R"(table "Table 1": 1 column added (column 3))");
    r.op = TableOp::ColumnsRemoved;
    r.indices = {2, 3};
    CHECK(describe(r) == R"(table "Table 1": 2 columns removed (columns 2, 3))");

    TableChange m;
    m.shape_name = "Table 1";
    m.op = TableOp::MergeChanged;
    m.a_row = 1;
    m.b_row = 1;
    m.col = 2;
    m.old_merge = pptctl::TableMerge{1, 1, false};
    m.new_merge = pptctl::TableMerge{1, 1, true};
    CHECK(describe(m) == R"(table "Table 1": cell (1,2) merge changed: 1x1 -> 1x1 (covered))");
}

TEST_CASE("Diff/table: to_string(TableOp) is stable snake_case") {
    CHECK(pptctl::to_string(TableOp::CellChanged) == "cell_changed");
    CHECK(pptctl::to_string(TableOp::RowsAdded) == "rows_added");
    CHECK(pptctl::to_string(TableOp::RowsRemoved) == "rows_removed");
    CHECK(pptctl::to_string(TableOp::ColumnsAdded) == "columns_added");
    CHECK(pptctl::to_string(TableOp::ColumnsRemoved) == "columns_removed");
    CHECK(pptctl::to_string(TableOp::MergeChanged) == "merge_changed");
}

TEST_CASE("Diff/table: to_json() carries every key, null when not applicable") {
    TableChange c;
    c.op = TableOp::CellChanged;
    c.shape_id = "2";
    c.shape_name = "Table 1";
    c.a_row = 2;
    c.b_row = 2;
    c.col = 1;
    c.old_text = "Q1";
    c.new_text = "Q2";
    const auto j = to_json(c);
    const std::vector<std::string> keys = {"op",
                                           "shape_id",
                                           "shape_name",
                                           "a_row",
                                           "b_row",
                                           "col",
                                           "indices",
                                           "old_text",
                                           "new_text",
                                           "old_merge",
                                           "new_merge"};
    for (const auto& k : keys) {
        CHECK_MESSAGE(j.contains(k), k);
    }
    CHECK(j["op"] == "cell_changed");
    CHECK(j["a_row"] == 2);
    CHECK(j["col"] == 1);
    CHECK(j["old_text"] == "Q1");
    CHECK(j["indices"].is_null());
    CHECK(j["old_merge"].is_null());

    TableChange r;
    r.op = TableOp::RowsAdded;
    r.indices = {3, 4};
    const auto jr = to_json(r);
    CHECK(jr["indices"] == nlohmann::json::array({3, 4}));
    CHECK(jr["a_row"].is_null());
    CHECK(jr["old_text"].is_null());

    TableChange m;
    m.op = TableOp::MergeChanged;
    m.old_merge = pptctl::TableMerge{1, 1, false};
    m.new_merge = pptctl::TableMerge{2, 1, false};
    const auto jm = to_json(m);
    CHECK(jm["new_merge"]["col_span"] == 2);
    CHECK(jm["new_merge"]["row_span"] == 1);
    CHECK(jm["new_merge"]["covered"] == false);
}

// ── renderer wiring ───────────────────────────────────────────────────────────

namespace {

pptctl::DiffReport table_chart_report() {
    pptctl::DiffReport r;
    pptctl::SlideDiff s;
    s.status = pptctl::SlideStatus::Changed;
    s.a_index = 1;
    s.b_index = 1;
    s.similarity = 0.9;

    TableChange t;
    t.op = TableOp::CellChanged;
    t.shape_name = "Table 1";
    t.a_row = 2;
    t.b_row = 2;
    t.col = 2;
    t.old_text = "100";
    t.new_text = "<120>";
    s.changes.tables.push_back(t);

    pptctl::ChartChange c;
    c.op = pptctl::ChartOp::TitleChanged;
    c.shape_name = "Chart 1";
    c.old_title = "Old";
    c.new_title = "New";
    s.changes.charts.push_back(c);

    r.slides.push_back(s);
    return r;
}

std::string render_as(const char* format) {
    std::ostringstream os;
    pptctl::DiffOutputOptions out;
    out.format = format;
    out.color = false;
    pptctl::cmd::render(table_chart_report(), "a.pptx", "b.pptx", os, out);
    return os.str();
}

}  // namespace

TEST_CASE("Diff/table: render — table and chart ops appear in every format") {
    const auto report = table_chart_report();
    const std::string u = render_as("unified");
    CHECK(u.find("  " + describe(report.slides[0].changes.tables[0]) + "\n") != std::string::npos);
    CHECK(u.find("  " + describe(report.slides[0].changes.charts[0]) + "\n") != std::string::npos);

    const auto j = nlohmann::json::parse(render_as("json"));
    const auto& c = j["slides"][0]["changes"];
    REQUIRE(c["tables"].size() == 1);
    CHECK(c["tables"][0]["op"] == "cell_changed");
    REQUIRE(c["charts"].size() == 1);
    CHECK(c["charts"][0]["op"] == "title_changed");

    const std::string h = render_as("html");
    CHECK(h.find("<li class=\"table\">") != std::string::npos);
    CHECK(h.find("<li class=\"chart\">") != std::string::npos);
    CHECK(h.find("&lt;120&gt;") != std::string::npos);
    CHECK(h.find("<120>") == std::string::npos);
}
