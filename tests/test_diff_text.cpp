// Tests for the text-level diff (Phase 4). Owned by the text diff issue; keep
// text-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <pptctl/diff.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

namespace {

using pptctl::TextChange;
using pptctl::TextChangeKind;

// One a:p carrying a single plain run.
std::string para(const std::string& text) {
    return "<a:p><a:r><a:t>" + text + "</a:t></a:r></a:p>";
}

// One a:r with explicit run properties (attributes/children of a:rPr).
std::string run(const std::string& text,
                const std::string& rpr_attrs,
                const std::string& rpr_children = "") {
    return "<a:r><a:rPr " + rpr_attrs + ">" + rpr_children + "</a:rPr><a:t>" + text +
           "</a:t></a:r>";
}

// A slide with one text shape (id 2) whose body is `paras` (concatenated a:p
// elements). Same skeleton as test_diff.cpp's text_slide so ids/names line up.
std::string body_slide(const std::string& paras, const char* name = "Title 1") {
    return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name=")") +
           name + R"("/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="100" y="200"/><a:ext cx="300" cy="400"/></a:xfrm></p:spPr>
      <p:txBody><a:bodyPr/><a:lstStyle/>)" +
           paras + R"(</p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
}

// A slide with one text shape whose single paragraph is `text`.
std::string text_slide(const std::string& text, const char* name = "Title 1") {
    return body_slide(para(text), name);
}

// A notesSlide whose body placeholder holds `paras`.
std::string notes_slide(const std::string& paras) {
    return std::string(R"(<?xml version="1.0"?>
<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
         xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="3" name="Notes Placeholder 2"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="body" idx="3"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:lstStyle/>)") +
           paras + R"(</p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:notes>)";
}

pptctl::Slide parse(const std::string& xml, const std::string& notes_xml = "") {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    if (!notes_xml.empty()) {
        REQUIRE(s.parse_notes(notes_xml));
    }
    return s;
}

std::vector<TextChange> text_diff(const std::string& a_xml,
                                  const std::string& b_xml,
                                  const pptctl::DiffOptions& opts = {}) {
    const auto a = parse(a_xml);
    const auto b = parse(b_xml);
    return pptctl::compare_slides(a, b, opts).text;
}

}  // namespace

TEST_CASE("Diff/text: identical slides produce no text changes") {
    const auto ops = text_diff(text_slide("Quarterly results"), text_slide("Quarterly results"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: editing one word is a changed paragraph with old and new text") {
    const auto ops = text_diff(text_slide("Quarterly results"), text_slide("Quarterly targets"));
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.kind == TextChangeKind::Changed);
    CHECK(c.shape == "Title 1");
    CHECK(c.shape_id == "2");
    CHECK_FALSE(c.notes);
    CHECK(c.a_paragraph == 1);
    CHECK(c.b_paragraph == 1);
    CHECK(c.old_text == "Quarterly results");
    CHECK(c.new_text == "Quarterly targets");
    CHECK(pptctl::describe(c) ==
          R"(shape "Title 1": paragraph 1 changed: "Quarterly results" -> "Quarterly targets")");
}

TEST_CASE("Diff/text: adding a bullet is an inserted paragraph") {
    const auto ops = text_diff(body_slide(para("one") + para("two")),
                               body_slide(para("one") + para("two") + para("three")));
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.kind == TextChangeKind::Inserted);
    CHECK(c.a_paragraph == 0);
    CHECK(c.b_paragraph == 3);
    CHECK(c.old_text.empty());
    CHECK(c.new_text == "three");
    CHECK(pptctl::describe(c) == R"(shape "Title 1": paragraph 3 inserted: "three")");
}

TEST_CASE("Diff/text: deleting a bullet is a deleted paragraph") {
    const auto ops = text_diff(body_slide(para("one") + para("two") + para("three")),
                               body_slide(para("one") + para("three")));
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.kind == TextChangeKind::Deleted);
    CHECK(c.a_paragraph == 2);
    CHECK(c.b_paragraph == 0);
    CHECK(c.old_text == "two");
    CHECK(c.new_text.empty());
    CHECK(pptctl::describe(c) == R"(shape "Title 1": paragraph 2 deleted: "two")");
}

TEST_CASE("Diff/text: LCS keeps surrounding paragraphs aligned across an insert") {
    // Insert in the middle: paragraphs after it keep matching by content, so
    // only the new one is reported (not a cascade of changes).
    const auto ops = text_diff(body_slide(para("one") + para("two") + para("three")),
                               body_slide(para("one") + para("new") + para("two") + para("three")));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == TextChangeKind::Inserted);
    CHECK(ops[0].b_paragraph == 2);
    CHECK(ops[0].new_text == "new");
}

TEST_CASE("Diff/text: turning bold on is a reformatted paragraph") {
    const auto ops =
        text_diff(body_slide("<a:p>" + run("Hello world", "lang=\"en-US\"") + "</a:p>"),
                  body_slide("<a:p>" + run("Hello world", "b=\"1\"") + "</a:p>"));
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.kind == TextChangeKind::Reformatted);
    CHECK(c.a_paragraph == 1);
    CHECK(c.b_paragraph == 1);
    CHECK(c.old_text == "Hello world");
    CHECK(c.new_text == "Hello world");
    CHECK(c.formatting == R"("Hello world": bold on)");
    CHECK(pptctl::describe(c) ==
          R"(shape "Title 1": paragraph 1 reformatted: "Hello world": bold on)");
}

TEST_CASE("Diff/text: reformatting only part of a paragraph names that span") {
    const auto ops = text_diff(body_slide("<a:p>" + run("Hello ", "lang=\"en-US\"") +
                                          run("world", "lang=\"en-US\"") + "</a:p>"),
                               body_slide("<a:p>" + run("Hello ", "lang=\"en-US\"") +
                                          run("world", "i=\"1\" sz=\"2400\"") + "</a:p>"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == TextChangeKind::Reformatted);
    CHECK(ops[0].formatting == R"("world": italic on, size inherit -> 24pt)");
}

TEST_CASE("Diff/text: changing the font is reported by name") {
    const auto ops = text_diff(
        body_slide("<a:p>" + run("Hi", "", "<a:latin typeface=\"Arial\"/>") + "</a:p>"),
        body_slide("<a:p>" + run("Hi", "", "<a:latin typeface=\"Calibri\"/>") + "</a:p>"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == TextChangeKind::Reformatted);
    CHECK(ops[0].formatting == R"("Hi": font "Arial" -> "Calibri")");
}

TEST_CASE("Diff/text: re-splitting runs without changing formatting is not a change") {
    const auto ops = text_diff(
        body_slide("<a:p>" + run("Hello world", "b=\"1\"") + "</a:p>"),
        body_slide("<a:p>" + run("Hello ", "b=\"1\"") + run("world", "b=\"1\"") + "</a:p>"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: editing speaker notes is reported against the notes, not a shape") {
    const auto a =
        parse(text_slide("Agenda"), notes_slide(para("Say hello") + para("Ask questions")));
    const auto b =
        parse(text_slide("Agenda"), notes_slide(para("Say hello") + para("Take questions")));
    const auto ops = pptctl::compare_slides(a, b, {}).text;
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.kind == TextChangeKind::Changed);
    CHECK(c.notes);
    CHECK(c.shape.empty());
    CHECK(c.a_paragraph == 2);
    CHECK(c.b_paragraph == 2);
    CHECK(c.old_text == "Ask questions");
    CHECK(c.new_text == "Take questions");
    CHECK(pptctl::describe(c) ==
          R"(notes: paragraph 2 changed: "Ask questions" -> "Take questions")");
}

TEST_CASE("Diff/text: whitespace-only differences are ignored by default") {
    const auto ops =
        text_diff(text_slide("Quarterly   results "), text_slide(" Quarterly results"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: an added empty paragraph is whitespace churn by default") {
    const auto ops = text_diff(body_slide(para("one")), body_slide(para("one") + "<a:p/>"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: field runs are ignored by default") {
    const std::string fld_a = R"(<a:fld id="{X}" type="slidenum"><a:t>1</a:t></a:fld>)";
    const std::string fld_b = R"(<a:fld id="{X}" type="slidenum"><a:t>2</a:t></a:fld>)";
    const auto ops = text_diff(body_slide("<a:p><a:r><a:t>Slide </a:t></a:r>" + fld_a + "</a:p>"),
                               body_slide("<a:p><a:r><a:t>Slide </a:t></a:r>" + fld_b + "</a:p>"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: strict_text reports whitespace and field-run differences") {
    pptctl::DiffOptions opts;
    opts.strict_text = true;

    SUBCASE("whitespace") {
        const auto ops =
            text_diff(text_slide("Quarterly  results"), text_slide("Quarterly results"), opts);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].kind == TextChangeKind::Changed);
        CHECK(ops[0].old_text == "Quarterly  results");
        CHECK(ops[0].new_text == "Quarterly results");
    }
    SUBCASE("field run") {
        const std::string fld_a = R"(<a:fld id="{X}" type="slidenum"><a:t>1</a:t></a:fld>)";
        const std::string fld_b = R"(<a:fld id="{X}" type="slidenum"><a:t>2</a:t></a:fld>)";
        const auto ops =
            text_diff(body_slide("<a:p><a:r><a:t>Slide </a:t></a:r>" + fld_a + "</a:p>"),
                      body_slide("<a:p><a:r><a:t>Slide </a:t></a:r>" + fld_b + "</a:p>"),
                      opts);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].old_text == "Slide 1");
        CHECK(ops[0].new_text == "Slide 2");
    }
    SUBCASE("empty paragraph") {
        const auto ops =
            text_diff(body_slide(para("one")), body_slide(para("one") + "<a:p/>"), opts);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].kind == TextChangeKind::Inserted);
        CHECK(ops[0].b_paragraph == 2);
    }
}

TEST_CASE("Diff/text: shapes are matched by name when ids differ") {
    std::string a_xml = text_slide("Hello", "Content 3");
    std::string b_xml = text_slide("Goodbye", "Content 3");
    const auto pos = b_xml.find(R"(id="2" name="Content 3")");
    REQUIRE(pos != std::string::npos);
    b_xml.replace(pos, 6, R"(id="9")");
    const auto ops = text_diff(a_xml, b_xml);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == TextChangeKind::Changed);
    CHECK(ops[0].shape == "Content 3");
    CHECK(ops[0].shape_id == "9");
}

TEST_CASE("Diff/text: shapes are matched by placeholder when ids and names differ") {
    const auto ph_slide = [](const char* id, const char* name, const std::string& text) {
        return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id=")") +
               id + R"(" name=")" + name +
               R"("/><p:cNvSpPr/><p:nvPr><p:ph type="body" idx="1"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:lstStyle/>)" +
               para(text) + R"(</p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
    };
    const auto ops = text_diff(ph_slide("4", "Content Placeholder 3", "Hello"),
                               ph_slide("7", "Text Placeholder 6", "Goodbye"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == TextChangeKind::Changed);
    CHECK(ops[0].shape == "Text Placeholder 6");
}

TEST_CASE("Diff/text: text inside group children is diffed") {
    const auto grouped = [](const std::string& text) {
        return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="5" name="Group 4"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr/>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="6" name="Label 5"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/><a:lstStyle/>)") +
               para(text) + R"(</p:txBody>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";
    };
    const auto ops = text_diff(grouped("inner"), grouped("outer"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].shape == "Label 5");
    CHECK(ops[0].old_text == "inner");
    CHECK(ops[0].new_text == "outer");
}

TEST_CASE("Diff/text: unmatched shapes are not reported here (they belong to the shape diff)") {
    const auto ops = text_diff(text_slide("Hello", "Title 1"), text_slide("Hello", "Other 9"));
    CHECK(ops.empty());
}

TEST_CASE("Diff/text: to_json uses stable snake_case keys") {
    const auto ops = text_diff(text_slide("Quarterly results"), text_slide("Quarterly targets"));
    REQUIRE(ops.size() == 1);
    const auto j = pptctl::to_json(ops[0]);
    CHECK(j["kind"] == "changed");
    CHECK(j["shape"] == "Title 1");
    CHECK(j["shape_id"] == "2");
    CHECK(j["notes"] == false);
    CHECK(j["a_paragraph"] == 1);
    CHECK(j["b_paragraph"] == 1);
    CHECK(j["old_text"] == "Quarterly results");
    CHECK(j["new_text"] == "Quarterly targets");
    CHECK(j["formatting"].is_null());

    const auto ins = text_diff(body_slide(para("one")), body_slide(para("one") + para("two")));
    REQUIRE(ins.size() == 1);
    const auto ji = pptctl::to_json(ins[0]);
    CHECK(ji["kind"] == "inserted");
    CHECK(ji["a_paragraph"].is_null());
    CHECK(ji["b_paragraph"] == 2);
    CHECK(ji["old_text"].is_null());
    CHECK(ji["new_text"] == "two");

    const auto fmt = text_diff(body_slide("<a:p>" + run("Hi", "") + "</a:p>"),
                               body_slide("<a:p>" + run("Hi", "u=\"sng\"") + "</a:p>"));
    REQUIRE(fmt.size() == 1);
    const auto jf = pptctl::to_json(fmt[0]);
    CHECK(jf["kind"] == "reformatted");
    CHECK(jf["formatting"] == R"("Hi": underline on)");
}
