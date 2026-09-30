// Tests for the shape-level diff (Phase 4). Owned by the shape diff issue; keep
// shape-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

namespace {

using pptctl::Emu;

// One p:sp with explicit id/name/geometry, an optional fill fragment (the raw
// child of p:spPr, e.g. <a:solidFill>...</a:solidFill>) and one text run.
std::string sp(const char* id,
               const char* name,
               Emu x,
               Emu y,
               Emu cx,
               Emu cy,
               const std::string& fill = "",
               const std::string& text = "hello") {
    return std::string(R"(<p:sp>
      <p:nvSpPr><p:cNvPr id=")") +
           id + R"(" name=")" + name + R"("/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x=")" +
           std::to_string(x) + R"(" y=")" + std::to_string(y) + R"("/><a:ext cx=")" +
           std::to_string(cx) + R"(" cy=")" + std::to_string(cy) + R"("/></a:xfrm>)" + fill +
           R"(</p:spPr>
      <p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>)" +
           text + R"(</a:t></a:r></a:p></p:txBody>
    </p:sp>)";
}

// A placeholder p:sp: like sp() but with p:ph type/idx and no explicit text.
std::string ph(const char* id, const char* name, const char* type, int idx, Emu x, Emu y) {
    return std::string(R"(<p:sp>
      <p:nvSpPr><p:cNvPr id=")") +
           id + R"(" name=")" + name + R"("/><p:cNvSpPr/><p:nvPr><p:ph type=")" + type +
           R"(" idx=")" + std::to_string(idx) + R"("/></p:nvPr></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x=")" +
           std::to_string(x) + R"(" y=")" + std::to_string(y) +
           R"("/><a:ext cx="300" cy="400"/></a:xfrm></p:spPr>
    </p:sp>)";
}

// A p:pic with the same identity/geometry attributes as sp().
std::string pic(const char* id, const char* name, Emu x, Emu y, Emu cx, Emu cy) {
    return std::string(R"(<p:pic>
      <p:nvPicPr><p:cNvPr id=")") +
           id + R"(" name=")" + name + R"("/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:embed="rId2"/></p:blipFill>
      <p:spPr><a:xfrm><a:off x=")" +
           std::to_string(x) + R"(" y=")" + std::to_string(y) + R"("/><a:ext cx=")" +
           std::to_string(cx) + R"(" cy=")" + std::to_string(cy) + R"("/></a:xfrm></p:spPr>
    </p:pic>)";
}

// A p:grpSp wrapping `children` (already-rendered shape fragments).
std::string grp(const char* id, const char* name, const std::string& children) {
    return std::string(R"(<p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id=")") +
           id + R"(" name=")" + name + R"("/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1000" cy="1000"/>
        <a:chOff x="0" y="0"/><a:chExt cx="1000" cy="1000"/></a:xfrm></p:grpSpPr>
      )" + children +
           R"(
    </p:grpSp>)";
}

// Same envelope as text_slide() in test_diff.cpp, but taking ready-made shape
// fragments so tests can vary geometry, fill, kind and nesting.
std::string slide(const std::vector<std::string>& shapes) {
    std::string xml = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
)";
    for (const auto& s : shapes) {
        xml += "    " + s + "\n";
    }
    xml += R"(  </p:spTree></p:cSld>
</p:sld>)";
    return xml;
}

pptctl::Slide parse(const std::string& xml) {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    return s;
}

std::vector<pptctl::ShapeChange> shape_diff(const std::string& a_xml,
                                            const std::string& b_xml,
                                            const pptctl::DiffOptions& opts = {}) {
    const auto a = parse(a_xml);
    const auto b = parse(b_xml);
    return pptctl::compare_slides(a, b, opts).shapes;
}

const std::string kRed = R"(<a:solidFill><a:srgbClr val="FF0000"/></a:solidFill>)";
const std::string kBlue = R"(<a:solidFill><a:srgbClr val="0000FF"/></a:solidFill>)";
const std::string kAccent1 = R"(<a:solidFill><a:schemeClr val="accent1"/></a:solidFill>)";
const std::string kAccent2 = R"(<a:solidFill><a:schemeClr val="accent2"/></a:solidFill>)";

}  // namespace

TEST_CASE("Diff/shape: scaffold compiles") {
    pptctl::SlideChanges c;
    CHECK(c.empty());
}

TEST_CASE("Diff/shape: identical slides yield no shape ops") {
    const auto a = slide({sp("2", "Title 1", 100, 200, 300, 400, kRed)});
    const auto ops = shape_diff(a, a);
    CHECK(ops.empty());
}

TEST_CASE("Diff/shape: nudging a shape reports Moved with the EMU delta and both geometries") {
    const auto a = slide({sp("2", "Title 1", 914400, 914400, 300, 400)});
    const auto b = slide({sp("2", "Title 1", 1143000, 822960, 300, 400)});  // +0.25 in, -0.10 in
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Moved);
    CHECK(op.id == "2");
    CHECK(op.name == "Title 1");
    CHECK(op.kind == pptctl::ShapeKind::TextBox);
    CHECK(op.delta_x == 228600);
    CHECK(op.delta_y == -91440);
    CHECK(op.delta_cx == 0);
    CHECK(op.delta_cy == 0);
    REQUIRE(op.old_geometry.has_value());
    REQUIRE(op.new_geometry.has_value());
    CHECK(op.old_geometry->off_x == 914400);
    CHECK(op.new_geometry->off_x == 1143000);
    CHECK(op.new_geometry->off_y == 822960);
    CHECK(op.group_path.empty());
}

TEST_CASE("Diff/shape: resizing a shape reports Resized with the extent delta") {
    const auto a = slide({sp("2", "Title 1", 100, 200, 914400, 914400)});
    const auto b = slide({sp("2", "Title 1", 100, 200, 1828800, 457200)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Resized);
    CHECK(op.delta_x == 0);
    CHECK(op.delta_y == 0);
    CHECK(op.delta_cx == 914400);
    CHECK(op.delta_cy == -457200);
    REQUIRE(op.old_geometry.has_value());
    REQUIRE(op.new_geometry.has_value());
    CHECK(op.old_geometry->ext_cx == 914400);
    CHECK(op.new_geometry->ext_cx == 1828800);
}

TEST_CASE("Diff/shape: moving and resizing at once yields one Moved and one Resized op") {
    const auto a = slide({sp("2", "Title 1", 0, 0, 100000, 100000)});
    const auto b = slide({sp("2", "Title 1", 50000, 0, 200000, 100000)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);
    CHECK(ops[1].op == pptctl::ShapeOp::Resized);
}

TEST_CASE("Diff/shape: changing an srgb fill reports Recolored with both fills") {
    const auto a = slide({sp("2", "Box", 100, 200, 300, 400, kRed)});
    const auto b = slide({sp("2", "Box", 100, 200, 300, 400, kBlue)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Recolored);
    REQUIRE(op.old_fill.has_value());
    REQUIRE(op.new_fill.has_value());
    CHECK(op.old_fill->kind == "solid");
    CHECK(op.old_fill->color == "FF0000");
    CHECK(op.new_fill->kind == "solid");
    CHECK(op.new_fill->color == "0000FF");
}

TEST_CASE("Diff/shape: changing a scheme fill reports Recolored with scheme names") {
    const auto a = slide({sp("2", "Box", 100, 200, 300, 400, kAccent1)});
    const auto b = slide({sp("2", "Box", 100, 200, 300, 400, kAccent2)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Recolored);
    REQUIRE(ops[0].old_fill.has_value());
    REQUIRE(ops[0].new_fill.has_value());
    CHECK(ops[0].old_fill->color == "accent1");
    CHECK(ops[0].new_fill->color == "accent2");
}

TEST_CASE("Diff/shape: adding or dropping an explicit fill is a recolor too") {
    const auto plain = slide({sp("2", "Box", 100, 200, 300, 400)});
    const auto red = slide({sp("2", "Box", 100, 200, 300, 400, kRed)});
    const auto none = slide({sp("2", "Box", 100, 200, 300, 400, "<a:noFill/>")});

    auto ops = shape_diff(plain, red);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Recolored);
    CHECK_FALSE(ops[0].old_fill.has_value());
    REQUIRE(ops[0].new_fill.has_value());
    CHECK(ops[0].new_fill->color == "FF0000");

    ops = shape_diff(red, none);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Recolored);
    REQUIRE(ops[0].new_fill.has_value());
    CHECK(ops[0].new_fill->kind == "none");
    CHECK(ops[0].new_fill->color.empty());
}

TEST_CASE("Diff/shape: a deleted shape is Removed with its old geometry") {
    const auto a =
        slide({sp("2", "Title 1", 100, 200, 300, 400), sp("3", "Box", 1, 2, 3, 4, kRed)});
    const auto b = slide({sp("2", "Title 1", 100, 200, 300, 400)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Removed);
    CHECK(op.id == "3");
    CHECK(op.name == "Box");
    REQUIRE(op.old_geometry.has_value());
    CHECK(op.old_geometry->off_x == 1);
    CHECK(op.old_geometry->ext_cy == 4);
    CHECK_FALSE(op.new_geometry.has_value());
    REQUIRE(op.old_fill.has_value());
    CHECK(op.old_fill->color == "FF0000");
}

TEST_CASE("Diff/shape: a new shape is Added with its new geometry and kind") {
    const auto a = slide({sp("2", "Title 1", 100, 200, 300, 400)});
    const auto b =
        slide({sp("2", "Title 1", 100, 200, 300, 400), pic("4", "Picture 3", 5, 6, 7, 8)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Added);
    CHECK(op.id == "4");
    CHECK(op.name == "Picture 3");
    CHECK(op.kind == pptctl::ShapeKind::Picture);
    CHECK_FALSE(op.old_geometry.has_value());
    REQUIRE(op.new_geometry.has_value());
    CHECK(op.new_geometry->off_x == 5);
    CHECK(op.new_geometry->ext_cy == 8);
}

TEST_CASE("Diff/shape: removals come before additions, in document order") {
    const auto a = slide({sp("2", "A", 0, 0, 1, 1), sp("3", "B", 0, 0, 1, 1)});
    const auto b = slide({sp("4", "C", 0, 0, 1, 1), sp("5", "D", 0, 0, 1, 1)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 4);
    CHECK(ops[0].op == pptctl::ShapeOp::Removed);
    CHECK(ops[0].name == "A");
    CHECK(ops[1].op == pptctl::ShapeOp::Removed);
    CHECK(ops[1].name == "B");
    CHECK(ops[2].op == pptctl::ShapeOp::Added);
    CHECK(ops[2].name == "C");
    CHECK(ops[3].op == pptctl::ShapeOp::Added);
    CHECK(ops[3].name == "D");
}

TEST_CASE("Diff/shape: jitter below the geometry tolerance is not reported") {
    pptctl::DiffOptions opts;
    CHECK(opts.geometry_tolerance_emu == 9525);
    const auto a = slide({sp("2", "Title 1", 914400, 914400, 914400, 914400)});
    // Every coordinate off by less than 1 px @ 96 DPI (9525 EMU).
    const auto b =
        slide({sp("2", "Title 1", 914400 + 9000, 914400 - 9525, 914400 + 1, 914400 - 9000)});
    CHECK(shape_diff(a, b, opts).empty());

    // Just over the threshold on one axis is reported.
    const auto c = slide({sp("2", "Title 1", 914400 + 9526, 914400, 914400, 914400)});
    const auto ops = shape_diff(a, c, opts);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);

    // Tolerance zero reports everything.
    opts.geometry_tolerance_emu = 0;
    CHECK(shape_diff(a, b, opts).size() == 2);
}

TEST_CASE("Diff/shape: a group child is matched inside its parent and carries the group path") {
    const auto a = slide({grp("2", "Group 1", sp("3", "Child", 100, 100, 50, 50))});
    const auto b = slide({grp("2", "Group 1", sp("3", "Child", 100, 914500, 50, 50))});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    const auto& op = ops[0];
    CHECK(op.op == pptctl::ShapeOp::Moved);
    CHECK(op.id == "3");
    CHECK(op.name == "Child");
    CHECK(op.delta_y == 914400);
    REQUIRE(op.group_path.size() == 1);
    CHECK(op.group_path[0] == "Group 1");
}

TEST_CASE("Diff/shape: a child removed from a group is Removed with the group path") {
    const auto a =
        slide({grp("2", "Group 1", sp("3", "Child", 0, 0, 1, 1) + sp("4", "Other", 0, 0, 1, 1))});
    const auto b = slide({grp("2", "Group 1", sp("3", "Child", 0, 0, 1, 1))});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Removed);
    CHECK(ops[0].name == "Other");
    REQUIRE(ops[0].group_path.size() == 1);
    CHECK(ops[0].group_path[0] == "Group 1");
}

TEST_CASE("Diff/shape: shapes are matched by name when their ids differ") {
    const auto a = slide({sp("2", "Logo", 0, 0, 100, 100)});
    const auto b = slide({sp("9", "Logo", 50000, 0, 100, 100)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);
    CHECK(ops[0].id == "9");  // identity reported from B
    CHECK(ops[0].name == "Logo");
}

TEST_CASE("Diff/shape: placeholders are matched by type/idx when id and name differ") {
    const auto a = slide({ph("2", "Title 1", "title", 0, 0, 0)});
    const auto b = slide({ph("7", "Title 9", "title", 0, 50000, 0)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);
    CHECK(ops[0].kind == pptctl::ShapeKind::Placeholder);
    CHECK(ops[0].delta_x == 50000);
}

TEST_CASE("Diff/shape: two unrelated text boxes are not paired just by kind") {
    const auto a = slide({sp("2", "A", 0, 0, 1, 1)});
    const auto b = slide({sp("3", "B", 0, 0, 1, 1)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == pptctl::ShapeOp::Removed);
    CHECK(ops[1].op == pptctl::ShapeOp::Added);
}

TEST_CASE("Diff/shape: a matched shape whose kind differs reports KindChanged") {
    const auto a = slide({sp("2", "Thing", 0, 0, 100, 100)});
    const auto b = slide({pic("2", "Thing", 0, 0, 100, 100)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::KindChanged);
    REQUIRE(ops[0].old_kind.has_value());
    CHECK(*ops[0].old_kind == pptctl::ShapeKind::TextBox);
    CHECK(ops[0].kind == pptctl::ShapeKind::Picture);
}

TEST_CASE("Diff/shape: fingerprint — a recolor counts as changed") {
    auto a = parse(slide({sp("2", "Box", 0, 0, 1, 1, kRed)}));
    auto b = parse(slide({sp("2", "Box", 0, 0, 1, 1, kBlue)}));
    auto c = parse(slide({sp("2", "Box", 0, 0, 1, 1)}));
    CHECK(pptctl::summarize(a).fingerprint != pptctl::summarize(b).fingerprint);
    CHECK(pptctl::summarize(a).fingerprint != pptctl::summarize(c).fingerprint);
}

TEST_CASE("Diff/shape: describe — one line per op, inches with two decimals") {
    const auto moved = shape_diff(slide({pic("4", "Picture 3", 914400, 914400, 300, 400)}),
                                  slide({pic("4", "Picture 3", 1143000, 822960, 300, 400)}));
    REQUIRE(moved.size() == 1);
    CHECK(pptctl::describe(moved[0]) == R"(shape "Picture 3" moved by (+0.25 in, -0.10 in))");

    const auto resized = shape_diff(slide({sp("2", "Box", 0, 0, 914400, 914400)}),
                                    slide({sp("2", "Box", 0, 0, 1828800, 457200)}));
    REQUIRE(resized.size() == 1);
    CHECK(pptctl::describe(resized[0]) ==
          R"(shape "Box" resized by (+1.00 in, -0.50 in) to 2.00 x 0.50 in)");

    const auto recolored = shape_diff(slide({sp("2", "Box", 0, 0, 1, 1, kRed)}),
                                      slide({sp("2", "Box", 0, 0, 1, 1, kAccent1)}));
    REQUIRE(recolored.size() == 1);
    CHECK(pptctl::describe(recolored[0]) == R"(shape "Box" recolored from #FF0000 to accent1)");

    const auto added =
        shape_diff(slide({}), slide({sp("2", "Box", 914400, 457200, 1828800, 914400)}));
    REQUIRE(added.size() == 1);
    CHECK(pptctl::describe(added[0]) ==
          R"(shape "Box" (textbox) added at (1.00 in, 0.50 in), 2.00 x 1.00 in)");

    const auto removed = shape_diff(slide({sp("2", "Box", 0, 0, 1, 1)}), slide({}));
    REQUIRE(removed.size() == 1);
    CHECK(pptctl::describe(removed[0]) == R"(shape "Box" (textbox) removed)");

    const auto kind =
        shape_diff(slide({sp("2", "Thing", 0, 0, 1, 1)}), slide({pic("2", "Thing", 0, 0, 1, 1)}));
    REQUIRE(kind.size() == 1);
    CHECK(pptctl::describe(kind[0]) == R"(shape "Thing" changed kind from textbox to picture)");

    const auto grouped = shape_diff(slide({grp("2", "G", sp("3", "Child", 0, 0, 1, 1))}),
                                    slide({grp("2", "G", sp("3", "Child", 914400, 0, 1, 1))}));
    REQUIRE(grouped.size() == 1);
    CHECK(pptctl::describe(grouped[0]) ==
          R"(shape "Child" in group "G" moved by (+1.00 in, +0.00 in))");
}

TEST_CASE("Diff/shape: to_json — stable snake_case keys, EMU integers plus inches") {
    const auto moved = shape_diff(slide({grp("2", "G", sp("3", "Child", 914400, 0, 300, 400))}),
                                  slide({grp("2", "G", sp("3", "Child", 1143000, 0, 300, 400))}));
    REQUIRE(moved.size() == 1);
    const auto j = pptctl::to_json(moved[0]);
    CHECK(j["op"] == "moved");
    CHECK(j["id"] == "3");
    CHECK(j["name"] == "Child");
    CHECK(j["kind"] == "textbox");
    CHECK(j["group_path"] == nlohmann::json::array({"G"}));
    CHECK(j["old_kind"].is_null());
    CHECK(j["old_geometry"]["x_emu"] == 914400);
    CHECK(j["old_geometry"]["x_in"] == doctest::Approx(1.0));
    CHECK(j["new_geometry"]["x_emu"] == 1143000);
    CHECK(j["new_geometry"]["x_in"] == doctest::Approx(1.25));
    CHECK(j["new_geometry"]["cx_emu"] == 300);
    CHECK(j["delta"]["x_emu"] == 228600);
    CHECK(j["delta"]["x_in"] == doctest::Approx(0.25));
    CHECK(j["delta"]["y_emu"] == 0);
    CHECK(j["delta"]["cx_emu"] == 0);
    CHECK(j["old_fill"].is_null());
    CHECK(j["new_fill"].is_null());

    const auto recolored = shape_diff(slide({sp("2", "Box", 0, 0, 1, 1, kRed)}),
                                      slide({sp("2", "Box", 0, 0, 1, 1, kAccent1)}));
    REQUIRE(recolored.size() == 1);
    const auto r = pptctl::to_json(recolored[0]);
    CHECK(r["op"] == "recolored");
    CHECK(r["delta"].is_null());
    CHECK(r["old_fill"]["kind"] == "solid");
    CHECK(r["old_fill"]["color"] == "FF0000");
    CHECK(r["new_fill"]["color"] == "accent1");

    const auto kind =
        shape_diff(slide({sp("2", "Thing", 0, 0, 1, 1)}), slide({pic("2", "Thing", 0, 0, 1, 1)}));
    REQUIRE(kind.size() == 1);
    const auto k = pptctl::to_json(kind[0]);
    CHECK(k["op"] == "kind_changed");
    CHECK(k["old_kind"] == "textbox");
    CHECK(k["kind"] == "picture");

    const auto removed = shape_diff(slide({sp("2", "Box", 0, 0, 1, 1)}), slide({}));
    REQUIRE(removed.size() == 1);
    const auto d = pptctl::to_json(removed[0]);
    CHECK(d["op"] == "removed");
    CHECK(d["new_geometry"].is_null());
    CHECK(d["old_geometry"]["cy_emu"] == 1);
}

// ── inherited placeholder geometry (#77 finding) ──────────────────────────────
//
// A placeholder without a:xfrm takes its position from the layout. Moving it
// in PowerPoint writes an explicit a:xfrm on the slide; the reverse (reset to
// layout) removes it. Neither side can give a delta without resolving the
// layout, but the change must still be reported, not silently dropped.

namespace {
std::string inherited_ph(const char* id, const char* name) {
    return std::string(R"(<p:sp>
      <p:nvSpPr><p:cNvPr id=")") +
           id + R"(" name=")" + name +
           R"("/><p:cNvSpPr/><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Title</a:t></a:r></a:p></p:txBody>
    </p:sp>)";
}
}  // namespace

TEST_CASE("Diff/shape: placeholder going from inherited to explicit position is Moved") {
    const auto a = slide({inherited_ph("2", "Title 1")});
    const auto b = slide({ph("2", "Title 1", "title", 0, 1316183, 0)});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);
    CHECK_FALSE(ops[0].old_geometry.has_value());
    REQUIRE(ops[0].new_geometry.has_value());
    CHECK(ops[0].new_geometry->off_x == 1316183);
    CHECK(ops[0].delta_x == 0);
    const auto text = pptctl::describe(ops[0]);
    CHECK(text.find("inherited") != std::string::npos);
    CHECK(text.find("1.44 in") != std::string::npos);
    const auto j = pptctl::to_json(ops[0]);
    CHECK(j["old_geometry"].is_null());
    CHECK(j["delta"].is_null());
}

TEST_CASE("Diff/shape: placeholder reset to the layout position is Moved the other way") {
    const auto a = slide({ph("2", "Title 1", "title", 0, 1316183, 0)});
    const auto b = slide({inherited_ph("2", "Title 1")});
    const auto ops = shape_diff(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == pptctl::ShapeOp::Moved);
    REQUIRE(ops[0].old_geometry.has_value());
    CHECK_FALSE(ops[0].new_geometry.has_value());
    CHECK(pptctl::describe(ops[0]).find("inherited") != std::string::npos);
}
