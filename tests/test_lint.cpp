// Tests for the lint subcommand.

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <pptctl/lint.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace pptctl::cmd {
int lint_to(const std::string& path, std::ostream& os);
std::vector<pptctl::Diagnostic> lint_slide(const pptctl::Slide& slide,
                                           const pptctl::CanvasSize& canvas);
std::set<std::string> slide_referenced_fonts(const pptctl::Slide& slide);
std::vector<pptctl::Diagnostic> check_font_risk(const std::set<std::string>& referenced,
                                                const std::vector<std::string>& embedded);
}  // namespace pptctl::cmd

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

int run(const char* fixture_name, std::string& out) {
    std::ostringstream oss;
    const int rc = pptctl::cmd::lint_to(fixture(fixture_name), oss);
    out = oss.str();
    return rc;
}

// Standard OOXML canvas: 10" × 7.5"
constexpr pptctl::Emu kW = 9'144'000;
constexpr pptctl::Emu kH = 6'858'000;
constexpr pptctl::CanvasSize kCanvas{kW, kH};

// Build minimal slide XML with one sp whose geometry is set by the arguments.
std::string slide_xml(pptctl::Emu off_x,
                      pptctl::Emu off_y,
                      pptctl::Emu ext_cx,
                      pptctl::Emu ext_cy,
                      const char* name = "Box 1") {
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="{}"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr>
        <a:xfrm><a:off x="{}" y="{}"/><a:ext cx="{}" cy="{}"/></a:xfrm>
      </p:spPr>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)",
        name,
        off_x,
        off_y,
        ext_cx,
        ext_cy);
}

// Build a slide with a group (fitting within canvas) containing one child sp.
std::string group_slide_xml(pptctl::Emu child_off_x,
                            pptctl::Emu child_off_y,
                            pptctl::Emu child_ext_cx,
                            pptctl::Emu child_ext_cy) {
    // The group itself always fits exactly within the canvas; only the child may overflow.
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr>
        <a:xfrm><a:off x="0" y="0"/><a:ext cx="{}" cy="{}"/></a:xfrm>
      </p:grpSpPr>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="3" name="Child 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr>
          <a:xfrm><a:off x="{}" y="{}"/><a:ext cx="{}" cy="{}"/></a:xfrm>
        </p:spPr>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)",
        kW,
        kH,
        child_off_x,
        child_off_y,
        child_ext_cx,
        child_ext_cy);
}

std::vector<pptctl::Diagnostic> lint_xml(const std::string& xml) {
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    slide.set_index(1);
    return pptctl::cmd::lint_slide(slide, kCanvas);
}

}  // namespace

// ── integration: clean fixtures produce no spurious overflow ──────────────────

TEST_CASE("lint: minimal.pptx — no findings") {
    std::string out;
    CHECK(run("minimal.pptx", out) == 0);
    CHECK(out.empty());
}

TEST_CASE("lint: three_slides.pptx — no overflow") {
    std::string out;
    CHECK(run("three_slides.pptx", out) == 0);
    CHECK(out.find("overflow") == std::string::npos);
}

// ── overflow rule: no-fire cases ──────────────────────────────────────────────

TEST_CASE("overflow: shape exactly fills canvas — no diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, kW, kH));
    CHECK(diags.empty());
}

TEST_CASE("overflow: shape entirely within canvas — no diagnostic") {
    auto diags = lint_xml(slide_xml(100, 100, kW - 200, kH - 200));
    CHECK(diags.empty());
}

TEST_CASE("overflow: shape at origin with zero size — no diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, 0, 0));
    CHECK(diags.empty());
}

TEST_CASE("overflow: shape without geometry — no diagnostic") {
    constexpr const char* kNoGeom = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="NoGeom"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
    pptctl::Slide slide;
    REQUIRE(slide.parse(kNoGeom));
    slide.set_index(1);
    auto diags = pptctl::cmd::lint_slide(slide, kCanvas);
    CHECK(diags.empty());
}

// ── overflow rule: fire cases ─────────────────────────────────────────────────

TEST_CASE("overflow: shape extends 1 EMU past right edge — diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, kW + 1, kH));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
    CHECK(diags[0].severity == pptctl::Severity::Warning);
    CHECK(diags[0].slide_index == 1);
    CHECK(diags[0].shape_name == "Box 1");
}

TEST_CASE("overflow: shape extends 1 EMU past bottom edge — diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, kW, kH + 1));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: shape offset past right edge — diagnostic") {
    // off_x = kW + 1, even a zero-width shape at x > canvas_width overflows
    auto diags = lint_xml(slide_xml(kW + 1, 0, 100, 100));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: negative off_x — diagnostic") {
    auto diags = lint_xml(slide_xml(-1, 0, kW, kH));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: negative off_y — diagnostic") {
    auto diags = lint_xml(slide_xml(0, -1, kW, kH));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: negative ext_cx — diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, -1, kH));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: negative ext_cy — diagnostic") {
    auto diags = lint_xml(slide_xml(0, 0, kW, -1));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
}

TEST_CASE("overflow: diagnostic message contains shape name") {
    auto diags = lint_xml(slide_xml(0, 0, kW + 1, kH, "OffCanvas Box"));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message.find("OffCanvas Box") != std::string::npos);
}

TEST_CASE("overflow: child shape in group overflows — diagnostic fires recursively") {
    // Group itself fits the canvas; only the child overflows.
    auto diags = lint_xml(group_slide_xml(0, 0, kW + 1, kH));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "overflow");
    CHECK(diags[0].shape_name == "Child 1");
}

// ── tiny-text rule helpers ────────────────────────────────────────────────────

// Build a slide with one text shape. sz_centipt == 0 means no sz attribute.
std::string slide_xml_with_text(int sz_centipt, const char* name = "Label 1") {
    const std::string sz_attr = sz_centipt > 0 ? std::format(R"( sz="{}")", sz_centipt) : "";
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="{}"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr>
        <a:xfrm><a:off x="0" y="0"/><a:ext cx="1000000" cy="500000"/></a:xfrm>
      </p:spPr>
      <p:txBody>
        <a:bodyPr/>
        <a:p><a:r><a:rPr{}/>
          <a:t>Hello</a:t>
        </a:r></a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)",
        name,
        sz_attr);
}

// ── tiny-text rule: no-fire cases ─────────────────────────────────────────────

TEST_CASE("tiny-text: run at exactly 12pt (1200 centipt) — no diagnostic") {
    auto diags = lint_xml(slide_xml_with_text(1200));
    auto tiny = std::count_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    CHECK(tiny == 0);
}

TEST_CASE("tiny-text: run at 24pt — no diagnostic") {
    auto diags = lint_xml(slide_xml_with_text(2400));
    auto tiny = std::count_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    CHECK(tiny == 0);
}

TEST_CASE("tiny-text: run with no explicit size (inherited) — no diagnostic") {
    auto diags = lint_xml(slide_xml_with_text(0));  // 0 = no sz attribute
    auto tiny = std::count_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    CHECK(tiny == 0);
}

// ── tiny-text rule: fire cases ────────────────────────────────────────────────

TEST_CASE("tiny-text: run at 11.99pt (1199 centipt) — diagnostic") {
    auto diags = lint_xml(slide_xml_with_text(1199));
    auto it = std::find_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    REQUIRE(it != diags.end());
    CHECK(it->severity == pptctl::Severity::Warning);
    CHECK(it->slide_index == 1);
    CHECK(it->shape_name == "Label 1");
}

TEST_CASE("tiny-text: run at 8pt (800 centipt) — diagnostic") {
    auto diags = lint_xml(slide_xml_with_text(800));
    auto it = std::find_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    REQUIRE(it != diags.end());
    CHECK(it->rule_id == "tiny-text");
}

TEST_CASE("tiny-text: diagnostic message contains shape name") {
    auto diags = lint_xml(slide_xml_with_text(800, "SmallCaption"));
    auto it = std::find_if(
        diags.begin(), diags.end(), [](const auto& d) { return d.rule_id == "tiny-text"; });
    REQUIRE(it != diags.end());
    CHECK(it->message.find("SmallCaption") != std::string::npos);
}

TEST_CASE("tiny-text: integration — minimal.pptx produces no tiny-text findings") {
    std::string out;
    run("minimal.pptx", out);
    CHECK(out.find("tiny-text") == std::string::npos);
}

// ── alt-text rule helpers ─────────────────────────────────────────────────────

// Build a slide with one picture. Pass nullptr to omit descr/title entirely;
// decorative adds the Microsoft adec:decorative extension marker.
std::string slide_xml_with_pic(const char* descr,
                               const char* title = nullptr,
                               bool decorative = false,
                               const char* name = "Picture 1") {
    const std::string descr_attr = descr ? std::format(R"( descr="{}")", descr) : "";
    const std::string title_attr = title ? std::format(R"( title="{}")", title) : "";
    const char* ext = decorative ? R"(
        <a:extLst><a:ext uri="{C183D7F6-B498-43B3-948B-1728B52AA6E4}">
          <adec:decorative xmlns:adec="http://schemas.microsoft.com/office/drawing/2017/decorative" val="1"/>
        </a:ext></a:extLst>)"
                                 : "";
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="2" name="{}"{}{}>{}</p:cNvPr><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:embed="rId2"/></p:blipFill>
      <p:spPr>
        <a:xfrm><a:off x="0" y="0"/><a:ext cx="1000000" cy="500000"/></a:xfrm>
      </p:spPr>
    </p:pic>
  </p:spTree></p:cSld>
</p:sld>)",
        name,
        descr_attr,
        title_attr,
        ext);
}

std::vector<pptctl::Diagnostic> alt_text_diags(const std::string& xml) {
    auto diags = lint_xml(xml);
    std::erase_if(diags, [](const auto& d) { return d.rule_id != "alt-text"; });
    return diags;
}

// ── alt-text rule: no-fire cases ──────────────────────────────────────────────

TEST_CASE("alt-text: picture with non-empty descr — no diagnostic") {
    CHECK(alt_text_diags(slide_xml_with_pic("A chart of revenue")).empty());
}

TEST_CASE("alt-text: picture with empty descr but non-empty title — no diagnostic") {
    CHECK(alt_text_diags(slide_xml_with_pic("", "Revenue chart")).empty());
}

TEST_CASE("alt-text: picture with title only (no descr attribute) — no diagnostic") {
    CHECK(alt_text_diags(slide_xml_with_pic(nullptr, "Revenue chart")).empty());
}

TEST_CASE("alt-text: decorative picture without descr — no diagnostic") {
    CHECK(alt_text_diags(slide_xml_with_pic(nullptr, nullptr, true)).empty());
}

TEST_CASE("alt-text: non-picture shape without descr — no diagnostic") {
    CHECK(alt_text_diags(slide_xml_with_text(2400)).empty());
}

// ── alt-text rule: fire cases ─────────────────────────────────────────────────

TEST_CASE("alt-text: picture with no descr and no title — diagnostic") {
    auto diags = alt_text_diags(slide_xml_with_pic(nullptr));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "alt-text");
    CHECK(diags[0].severity == pptctl::Severity::Warning);
    CHECK(diags[0].slide_index == 1);
    CHECK(diags[0].shape_name == "Picture 1");
}

TEST_CASE("alt-text: picture with empty descr and empty title — diagnostic") {
    auto diags = alt_text_diags(slide_xml_with_pic("", ""));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "alt-text");
}

TEST_CASE("alt-text: diagnostic message contains shape name") {
    auto diags = alt_text_diags(slide_xml_with_pic(nullptr, nullptr, false, "Logo 7"));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message.find("Logo 7") != std::string::npos);
}

TEST_CASE("alt-text: integration — minimal.pptx produces no alt-text findings") {
    std::string out;
    run("minimal.pptx", out);
    CHECK(out.find("alt-text") == std::string::npos);
}

// ── empty-ph rule helpers ─────────────────────────────────────────────────────

// Build a slide with one placeholder shape. type/idx == nullptr/-1 omit the
// attribute; body == nullptr omits txBody entirely.
std::string slide_xml_with_ph(const char* type,
                              int idx,
                              const char* body,
                              const char* name = "Title 1") {
    const std::string type_attr = type ? std::format(R"( type="{}")", type) : "";
    const std::string idx_attr = idx >= 0 ? std::format(R"( idx="{}")", idx) : "";
    const std::string tx_body =
        body ? std::format("<p:txBody><a:bodyPr/><a:p><a:r><a:t>{}</a:t></a:r></a:p></p:txBody>",
                           body)
             : "";
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="{}"/><p:cNvSpPr/>
        <p:nvPr><p:ph{}{}/></p:nvPr>
      </p:nvSpPr>
      <p:spPr/>
      {}
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)",
        name,
        type_attr,
        idx_attr,
        tx_body);
}

std::vector<pptctl::Diagnostic> empty_ph_diags(const std::string& xml) {
    auto diags = lint_xml(xml);
    std::erase_if(diags, [](const auto& d) { return d.rule_id != "empty-ph"; });
    return diags;
}

// ── empty-ph rule: no-fire cases ──────────────────────────────────────────────

TEST_CASE("empty-ph: placeholder with text — no diagnostic") {
    CHECK(empty_ph_diags(slide_xml_with_ph("title", 0, "Quarterly results")).empty());
}

TEST_CASE("empty-ph: non-placeholder text box with no text — no diagnostic") {
    // slide_xml() builds a plain sp without p:ph and without txBody
    CHECK(empty_ph_diags(slide_xml(0, 0, 100, 100)).empty());
}

TEST_CASE("empty-ph: slide-number placeholder with no text — no diagnostic") {
    CHECK(empty_ph_diags(slide_xml_with_ph("sldNum", 10, nullptr)).empty());
}

TEST_CASE("empty-ph: date placeholder with no text — no diagnostic") {
    CHECK(empty_ph_diags(slide_xml_with_ph("dt", 11, nullptr)).empty());
}

TEST_CASE("empty-ph: footer placeholder with no text — no diagnostic") {
    CHECK(empty_ph_diags(slide_xml_with_ph("ftr", 12, nullptr)).empty());
}

// ── empty-ph rule: fire cases ─────────────────────────────────────────────────

TEST_CASE("empty-ph: title placeholder with no txBody — diagnostic") {
    auto diags = empty_ph_diags(slide_xml_with_ph("title", -1, nullptr));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "empty-ph");
    CHECK(diags[0].severity == pptctl::Severity::Warning);
    CHECK(diags[0].slide_index == 1);
    CHECK(diags[0].shape_name == "Title 1");
}

TEST_CASE("empty-ph: body placeholder with empty paragraph — diagnostic") {
    auto diags = empty_ph_diags(slide_xml_with_ph("body", 1, ""));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "empty-ph");
}

TEST_CASE("empty-ph: placeholder with whitespace-only text — diagnostic") {
    auto diags = empty_ph_diags(slide_xml_with_ph("body", 1, "  \t "));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "empty-ph");
}

TEST_CASE("empty-ph: untyped placeholder (defaults to body) with no text — diagnostic") {
    auto diags = empty_ph_diags(slide_xml_with_ph(nullptr, 1, nullptr));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "empty-ph");
}

TEST_CASE("empty-ph: diagnostic message contains placeholder type and idx") {
    auto diags = empty_ph_diags(slide_xml_with_ph("body", 3, nullptr, "Content 4"));
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message.find("Content 4") != std::string::npos);
    CHECK(diags[0].message.find("body") != std::string::npos);
    CHECK(diags[0].message.find('3') != std::string::npos);
}

TEST_CASE("empty-ph: integration — minimal.pptx produces no empty-ph findings") {
    std::string out;
    run("minimal.pptx", out);
    CHECK(out.find("empty-ph") == std::string::npos);
}

// ── font-risk rule helpers ────────────────────────────────────────────────────

// Build a slide with one text run carrying the given a:latin / a:ea / a:cs
// typefaces. Pass nullptr to omit an element.
std::string slide_xml_with_font(const char* latin,
                                const char* ea = nullptr,
                                const char* cs = nullptr) {
    const std::string latin_el = latin ? std::format(R"(<a:latin typeface="{}"/>)", latin) : "";
    const std::string ea_el = ea ? std::format(R"(<a:ea typeface="{}"/>)", ea) : "";
    const std::string cs_el = cs ? std::format(R"(<a:cs typeface="{}"/>)", cs) : "";
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Label 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/>
        <a:p><a:r><a:rPr>{}{}{}</a:rPr><a:t>Hi</a:t></a:r></a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)",
        latin_el,
        ea_el,
        cs_el);
}

std::set<std::string> referenced_fonts(const std::string& xml) {
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    return pptctl::cmd::slide_referenced_fonts(slide);
}

// ── font-risk: font collection ────────────────────────────────────────────────

TEST_CASE("font-risk: collects latin, ea and cs typefaces from a run") {
    auto fonts = referenced_fonts(slide_xml_with_font("Acme Sans", "Acme Mincho", "Acme Arabic"));
    CHECK(fonts.count("Acme Sans") == 1);
    CHECK(fonts.count("Acme Mincho") == 1);
    CHECK(fonts.count("Acme Arabic") == 1);
}

TEST_CASE("font-risk: ignores theme-font references (+mn-lt)") {
    auto fonts = referenced_fonts(slide_xml_with_font("+mn-lt", "+mn-ea", "+mn-cs"));
    CHECK(fonts.empty());
}

TEST_CASE("font-risk: collects a font from a run nested inside a group") {
    constexpr const char* kGrouped = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr/>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="3" name="Child 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/>
          <a:p><a:r><a:rPr><a:latin typeface="Nested Sans"/></a:rPr><a:t>Hi</a:t></a:r></a:p>
        </p:txBody>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";
    CHECK(referenced_fonts(kGrouped).count("Nested Sans") == 1);
}

// ── font-risk: check_font_risk no-fire cases ──────────────────────────────────

TEST_CASE("font-risk: safe-list font is not flagged") {
    CHECK(pptctl::cmd::check_font_risk({"Arial"}, {}).empty());
}

TEST_CASE("font-risk: safe-list match is case-insensitive") {
    CHECK(pptctl::cmd::check_font_risk({"TIMES NEW ROMAN"}, {}).empty());
}

TEST_CASE("font-risk: embedded font is not flagged (case-insensitive)") {
    CHECK(pptctl::cmd::check_font_risk({"Acme Sans"}, {"acme sans"}).empty());
}

// ── font-risk: check_font_risk fire cases ─────────────────────────────────────

TEST_CASE("font-risk: custom font, not embedded, not safe — diagnostic") {
    auto diags = pptctl::cmd::check_font_risk({"Acme Sans"}, {});
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "font-risk");
    CHECK(diags[0].severity == pptctl::Severity::Warning);
    CHECK(diags[0].slide_index == 0);  // deck-level
    CHECK(diags[0].shape_name.empty());
    CHECK(diags[0].message.find("Acme Sans") != std::string::npos);
}

TEST_CASE("font-risk: one diagnostic per at-risk face, deterministic order") {
    auto diags = pptctl::cmd::check_font_risk({"Zeta Sans", "Acme Sans"}, {});
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].message.find("Acme Sans") != std::string::npos);
    CHECK(diags[1].message.find("Zeta Sans") != std::string::npos);
}

TEST_CASE("font-risk: integration — minimal.pptx produces no font-risk findings") {
    std::string out;
    run("minimal.pptx", out);
    CHECK(out.find("font-risk") == std::string::npos);
}
