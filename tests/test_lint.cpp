// Tests for the lint subcommand.

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/image.hpp>
#include <pptctl/lint.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef PPTCTL_GOLDEN_DIR
#define PPTCTL_GOLDEN_DIR "tests/golden"
#endif

namespace pptctl::cmd {
int lint_to(const std::string& path, std::ostream& os, const pptctl::LintOptions& opts);
int report(const std::string& path,
           std::vector<pptctl::Diagnostic> diagnostics,
           const pptctl::LintOptions& opts,
           std::ostream& os);
std::vector<pptctl::Diagnostic> lint_slide(const pptctl::Slide& slide,
                                           const pptctl::CanvasSize& canvas);
std::set<std::string> slide_referenced_fonts(const pptctl::Slide& slide);
std::vector<pptctl::Diagnostic> check_font_risk(const std::set<std::string>& referenced,
                                                const std::vector<std::string>& embedded);
std::vector<pptctl::Diagnostic> check_ctype(
    const std::vector<std::pair<std::string, std::string>>& parts);
using ImageProbe = std::function<std::optional<pptctl::ImageDimensions>(const pptctl::Picture&)>;
std::vector<pptctl::Diagnostic> check_img_bloat(const pptctl::Slide& slide,
                                                const ImageProbe& probe);
}  // namespace pptctl::cmd

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

int run(const char* fixture_name, std::string& out, pptctl::LintOptions opts = {}) {
    std::ostringstream oss;
    const int rc = pptctl::cmd::lint_to(fixture(fixture_name), oss, opts);
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

TEST_CASE("lint: minimal.pptx — no slide-level findings") {
    std::string out;
    CHECK(run("minimal.pptx", out) == 0);
    CHECK(out.find("overflow") == std::string::npos);
    CHECK(out.find("tiny-text") == std::string::npos);
    CHECK(out.find("alt-text") == std::string::npos);
    CHECK(out.find("empty-ph") == std::string::npos);
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

// ── ctype rule helpers ────────────────────────────────────────────────────────

using Parts = std::vector<std::pair<std::string, std::string>>;

std::vector<pptctl::Diagnostic> ctype_diags(const Parts& parts) {
    return pptctl::cmd::check_ctype(parts);
}

// ── ctype rule: no-fire cases ─────────────────────────────────────────────────

TEST_CASE("ctype: normal slide XML part — no diagnostic") {
    Parts parts{{"ppt/slides/slide1.xml",
                 "application/vnd.openxmlformats-officedocument.presentationml.slide+xml"}};
    CHECK(ctype_diags(parts).empty());
}

TEST_CASE("ctype: [Content_Types].xml itself — not flagged as undeclared") {
    Parts parts{{"[Content_Types].xml", ""}};
    CHECK(ctype_diags(parts).empty());
}

TEST_CASE("ctype: relationship file with declared content type — not flagged") {
    Parts parts{{"_rels/.rels", "application/vnd.openxmlformats-package.relationships+xml"},
                {"ppt/_rels/presentation.xml.rels",
                 "application/vnd.openxmlformats-package.relationships+xml"}};
    CHECK(ctype_diags(parts).empty());
}

TEST_CASE("ctype: relationship file with empty content type — not flagged as undeclared") {
    // is_infrastructure() must suppress the diagnostic even when content_type() returns ""
    // (which can happen in packages where the .rels Default entry is missing).
    Parts parts{{"_rels/.rels", ""}, {"ppt/_rels/presentation.xml.rels", ""}};
    CHECK(ctype_diags(parts).empty());
}

TEST_CASE("ctype: directory entry — not flagged") {
    Parts parts{{"ppt/slides/", ""}};
    CHECK(ctype_diags(parts).empty());
}

// ── ctype rule: fire cases — suspect content types ────────────────────────────

TEST_CASE("ctype: printer-settings part — diagnostic") {
    Parts parts{{"ppt/printerSettings/printerSettings1.bin",
                 "application/vnd.openxmlformats-officedocument.presentationml.printerSettings"}};
    auto diags = ctype_diags(parts);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "ctype");
    CHECK(diags[0].severity == pptctl::Severity::Warning);
    CHECK(diags[0].slide_index == 0);
    CHECK(diags[0].shape_name.empty());
    CHECK(diags[0].message.find("printerSettings1.bin") != std::string::npos);
}

TEST_CASE("ctype: activeX binary part — diagnostic") {
    Parts parts{{"ppt/activeX/activeX1.bin", "application/vnd.ms-office.activeX"}};
    auto diags = ctype_diags(parts);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "ctype");
}

TEST_CASE("ctype: activeX XML part — diagnostic") {
    Parts parts{{"ppt/activeX/activeX1.xml", "application/vnd.ms-office.activeX+xml"}};
    CHECK(ctype_diags(parts).size() == 1);
}

TEST_CASE("ctype: VBA project part — diagnostic") {
    Parts parts{{"ppt/vbaProject.bin", "application/vnd.ms-office.vbaProject"}};
    auto diags = ctype_diags(parts);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "ctype");
}

TEST_CASE("ctype: OLE object part — diagnostic") {
    Parts parts{{"ppt/embeddings/oleObject1.bin",
                 "application/vnd.openxmlformats-officedocument.oleObject"}};
    CHECK(ctype_diags(parts).size() == 1);
}

TEST_CASE("ctype: diagnostic message contains part name and content type") {
    Parts parts{{"ppt/printerSettings/printerSettings1.bin",
                 "application/vnd.openxmlformats-officedocument.presentationml.printerSettings"}};
    auto diags = ctype_diags(parts);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message.find("printerSettings1.bin") != std::string::npos);
    CHECK(diags[0].message.find("printerSettings") != std::string::npos);
}

// ── ctype rule: fire cases — undeclared parts ─────────────────────────────────

TEST_CASE("ctype: part with no declared content type — diagnostic") {
    Parts parts{{"ppt/customXml/item1.xml", ""}};
    auto diags = ctype_diags(parts);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].rule_id == "ctype");
    CHECK(diags[0].message.find("item1.xml") != std::string::npos);
}

TEST_CASE("ctype: multiple suspect parts — one diagnostic per part") {
    Parts parts{
        {"ppt/printerSettings/printerSettings1.bin",
         "application/vnd.openxmlformats-officedocument.presentationml.printerSettings"},
        {"ppt/printerSettings/printerSettings2.bin",
         "application/vnd.openxmlformats-officedocument.presentationml.printerSettings"},
    };
    CHECK(ctype_diags(parts).size() == 2);
}

// ── ctype rule: integration ───────────────────────────────────────────────────

TEST_CASE("ctype: integration — minimal.pptx printer-settings flagged") {
    // Real PowerPoint files typically carry a printerSettings stub; minimal.pptx
    // does, so the ctype rule should fire exactly once for it.
    std::string out;
    CHECK(run("minimal.pptx", out) == 0);
    CHECK(out.find("ctype") != std::string::npos);
    CHECK(out.find("printerSettings") != std::string::npos);
}

TEST_CASE("ctype: integration — three_slides.pptx printer-settings flagged") {
    std::string out;
    CHECK(run("three_slides.pptx", out) == 0);
    CHECK(out.find("ctype") != std::string::npos);
    CHECK(out.find("printerSettings") != std::string::npos);
}

// ── img-bloat rule ────────────────────────────────────────────────────────────

namespace {

// One picture (optionally linked, optionally nested in a group) with the given
// display extent. 914400 EMU = 1 inch.
std::string bloat_pic_xml(pptctl::Emu cx,
                          pptctl::Emu cy,
                          bool linked = false,
                          bool grouped = false) {
    const std::string pic = std::format(
        R"(<p:pic>
      <p:nvPicPr><p:cNvPr id="3" name="Photo 7"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip {}="rId2"/></p:blipFill>
      <p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{}" cy="{}"/></a:xfrm></p:spPr>
    </p:pic>)",
        linked ? "r:link" : "r:embed",
        cx,
        cy);
    const std::string body = grouped ? std::format(
                                           R"(<p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{}" cy="{}"/></a:xfrm></p:grpSpPr>
      {}
    </p:grpSp>)",
                                           kW,
                                           kH,
                                           pic)
                                     : pic;
    return std::format(
        R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    {}
  </p:spTree></p:cSld>
</p:sld>)",
        body);
}

constexpr pptctl::Emu kInch = pptctl::kEmuPerInch;

// Runs the rule with a probe that always reports the given pixel size and
// counts how often it was consulted.
std::vector<pptctl::Diagnostic> bloat_diags(const std::string& xml,
                                            std::optional<pptctl::ImageDimensions> dims,
                                            int* probe_calls = nullptr) {
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    slide.set_index(4);
    return pptctl::cmd::check_img_bloat(slide, [&](const pptctl::Picture&) {
        if (probe_calls) {
            ++*probe_calls;
        }
        return dims;
    });
}

}  // namespace

TEST_CASE("img-bloat: 1600x1200 px shown at 1 inch wide — diagnostic") {
    const auto d = bloat_diags(bloat_pic_xml(kInch, kInch * 3 / 4), {{1600, 1200}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "img-bloat");
    CHECK(d[0].severity == pptctl::Severity::Warning);
    CHECK(d[0].slide_index == 4);
    CHECK(d[0].shape_name == "Photo 7");
}

TEST_CASE("img-bloat: exactly 200 DPI — no diagnostic (threshold is exclusive)") {
    CHECK(bloat_diags(bloat_pic_xml(kInch, kInch), {{200, 200}}).empty());
}

TEST_CASE("img-bloat: 201 DPI — diagnostic") {
    CHECK(bloat_diags(bloat_pic_xml(kInch, kInch), {{201, 200}}).size() == 1);
}

TEST_CASE("img-bloat: only the vertical axis is oversampled — diagnostic (max of both axes)") {
    CHECK(bloat_diags(bloat_pic_xml(kInch, kInch), {{100, 2000}}).size() == 1);
}

TEST_CASE("img-bloat: small image shown large — no diagnostic") {
    CHECK(bloat_diags(bloat_pic_xml(kInch * 8, kInch * 6), {{64, 48}}).empty());
}

TEST_CASE("img-bloat: zero-width extent — no diagnostic and probe not consulted") {
    int calls = 0;
    CHECK(bloat_diags(bloat_pic_xml(0, kInch), {{4000, 3000}}, &calls).empty());
    CHECK(calls == 0);
}

TEST_CASE("img-bloat: linked (external) picture — no diagnostic and probe not consulted") {
    int calls = 0;
    CHECK(
        bloat_diags(bloat_pic_xml(kInch, kInch, /*linked=*/true), {{4000, 3000}}, &calls).empty());
    CHECK(calls == 0);
}

TEST_CASE("img-bloat: probe cannot read the image — no diagnostic") {
    CHECK(bloat_diags(bloat_pic_xml(kInch, kInch), std::nullopt).empty());
}

TEST_CASE("img-bloat: non-picture shape — probe not consulted") {
    int calls = 0;
    CHECK(bloat_diags(slide_xml(0, 0, kInch, kInch), {{4000, 3000}}, &calls).empty());
    CHECK(calls == 0);
}

TEST_CASE("img-bloat: picture nested in a group — diagnostic fires recursively") {
    const auto d =
        bloat_diags(bloat_pic_xml(kInch, kInch, false, /*grouped=*/true), {{4000, 3000}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].shape_name == "Photo 7");
}

TEST_CASE("img-bloat: message reports pixel size, display size and effective DPI") {
    const auto d = bloat_diags(bloat_pic_xml(kInch * 2, kInch * 3 / 2), {{4000, 3000}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find("Photo 7") != std::string::npos);
    CHECK(d[0].message.find("4000x3000 px") != std::string::npos);
    CHECK(d[0].message.find("2.0x1.5 in") != std::string::npos);
    CHECK(d[0].message.find("2000 DPI") != std::string::npos);
    CHECK(d[0].message.find("200 DPI") != std::string::npos);
}

// ── img-bloat: integration ────────────────────────────────────────────────────

TEST_CASE("img-bloat: integration — bloat_deck.pptx flags the oversampled picture only") {
    std::string out;
    REQUIRE(run("bloat_deck.pptx", out) == 0);
    std::size_t hits = 0;
    for (std::size_t pos = out.find("[img-bloat]"); pos != std::string::npos;
         pos = out.find("[img-bloat]", pos + 1)) {
        ++hits;
    }
    CHECK(hits == 1);
    CHECK(out.find("bloat_deck.pptx:1: warning[img-bloat]") != std::string::npos);
    CHECK(out.find("1600x1200 px") != std::string::npos);
}

TEST_CASE("img-bloat: integration — images_deck.pptx (64 px images at 3 in) produces no findings") {
    std::string out;
    REQUIRE(run("images_deck.pptx", out) == 0);
    CHECK(out.find("[img-bloat]") == std::string::npos);
}

TEST_CASE("img-bloat: integration — minimal.pptx produces no findings") {
    std::string out;
    REQUIRE(run("minimal.pptx", out) == 0);
    CHECK(out.find("[img-bloat]") == std::string::npos);
}

// ── output & exit codes (#46) ─────────────────────────────────────────────────

namespace {

using pptctl::Diagnostic;
using pptctl::LintOptions;
using pptctl::Severity;

// Deliberately out of order: slide 2, deck-level, slide 1, slide 2 again.
std::vector<Diagnostic> mixed_diags() {
    return {
        {"tiny-text", Severity::Warning, 2, "Box A", "text too small"},
        {"ctype", Severity::Warning, 0, "", "suspect part"},
        {"overflow", Severity::Warning, 1, "Box B", "off canvas"},
        {"alt-text", Severity::Warning, 2, "Pic C", "no alt text"},
    };
}

struct Rendered {
    int rc = 0;
    std::string out;
};

Rendered render(std::vector<Diagnostic> diags, LintOptions opts = {}) {
    std::ostringstream oss;
    Rendered r;
    r.rc = pptctl::cmd::report("deck.pptx", std::move(diags), opts, oss);
    r.out = oss.str();
    return r;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

}  // namespace

// ── human-readable ────────────────────────────────────────────────────────────

TEST_CASE("output: no diagnostics — prints 'no issues found', exit 0") {
    const auto r = render({});
    CHECK(r.rc == 0);
    CHECK(r.out == "deck.pptx: no issues found\n");
}

TEST_CASE("output: deck-level diagnostics come first, then slides in ascending order") {
    const auto l = lines(render(mixed_diags()).out);
    REQUIRE(l.size() == 5);  // 4 diagnostics + summary
    CHECK(l[0].find("[ctype]") != std::string::npos);
    CHECK(l[1].find("[overflow]") != std::string::npos);
    CHECK(l[2].find("[tiny-text]") != std::string::npos);
    CHECK(l[3].find("[alt-text]") != std::string::npos);
}

TEST_CASE("output: ordering is stable within a slide") {
    // tiny-text was emitted before alt-text on slide 2; that order must survive.
    const auto l = lines(render(mixed_diags()).out);
    CHECK(l[2].find("[tiny-text]") != std::string::npos);
    CHECK(l[3].find("[alt-text]") != std::string::npos);
}

TEST_CASE("output: deck-level line has no slide number") {
    const auto l = lines(render(mixed_diags()).out);
    CHECK(l[0] == "deck.pptx: warning[ctype]: suspect part");
}

TEST_CASE("output: slide-level line is compiler-style file:slide:") {
    const auto l = lines(render(mixed_diags()).out);
    CHECK(l[1] == "deck.pptx:1: warning[overflow]: off canvas");
}

TEST_CASE("output: summary line counts warnings") {
    const auto l = lines(render(mixed_diags()).out);
    CHECK(l.back() == "deck.pptx: 4 warnings");
}

TEST_CASE("output: summary uses singular for one warning") {
    const auto l = lines(render({{"ctype", Severity::Warning, 0, "", "x"}}).out);
    CHECK(l.back() == "deck.pptx: 1 warning");
}

TEST_CASE("output: summary reports errors separately when present") {
    const auto l = lines(render({{"ctype", Severity::Warning, 0, "", "x"},
                                 {"ctype", Severity::Error, 0, "", "y"},
                                 {"ctype", Severity::Error, 1, "", "z"}})
                             .out);
    CHECK(l.back() == "deck.pptx: 1 warning, 2 errors");
}

TEST_CASE("output: error severity label appears on its line") {
    const auto l = lines(render({{"ctype", Severity::Error, 3, "S", "boom"}}).out);
    CHECK(l[0] == "deck.pptx:3: error[ctype]: boom");
}

// ── exit codes ────────────────────────────────────────────────────────────────

TEST_CASE("exit: diagnostics without --strict — exit 0") {
    CHECK(render(mixed_diags()).rc == 0);
}

TEST_CASE("exit: diagnostics with --strict — exit 2") {
    CHECK(render(mixed_diags(), {.strict = true}).rc == 2);
}

TEST_CASE("exit: clean with --strict — exit 0") {
    CHECK(render({}, {.strict = true}).rc == 0);
}

TEST_CASE("exit: --strict applies to JSON output too") {
    CHECK(render(mixed_diags(), {.json = true, .strict = true}).rc == 2);
}

// ── JSON ──────────────────────────────────────────────────────────────────────

TEST_CASE("json: root carries schema_version 1, file, diagnostics and summary") {
    const auto r = render(mixed_diags(), {.json = true});
    CHECK(r.rc == 0);
    const auto j = nlohmann::json::parse(r.out);
    CHECK(j["schema_version"] == 1);
    CHECK(j["file"] == "deck.pptx");
    REQUIRE(j["diagnostics"].is_array());
    CHECK(j["diagnostics"].size() == 4);
    CHECK(j["summary"]["warnings"] == 4);
    CHECK(j["summary"]["errors"] == 0);
}

TEST_CASE("json: diagnostic fields and ordering match the human output") {
    const auto j = nlohmann::json::parse(render(mixed_diags(), {.json = true}).out);
    const auto& d = j["diagnostics"];
    CHECK(d[0]["rule"] == "ctype");
    CHECK(d[1]["rule"] == "overflow");
    CHECK(d[2]["rule"] == "tiny-text");
    CHECK(d[3]["rule"] == "alt-text");
    CHECK(d[1]["severity"] == "warning");
    CHECK(d[1]["slide"] == 1);
    CHECK(d[1]["shape"] == "Box B");
    CHECK(d[1]["message"] == "off canvas");
}

TEST_CASE("json: deck-level diagnostic has null slide and null shape") {
    const auto j = nlohmann::json::parse(render(mixed_diags(), {.json = true}).out);
    CHECK(j["diagnostics"][0]["slide"].is_null());
    CHECK(j["diagnostics"][0]["shape"].is_null());
}

TEST_CASE("json: error severity is serialised as \"error\" and counted") {
    const auto j = nlohmann::json::parse(
        render({{"ctype", Severity::Error, 1, "", "boom"}}, {.json = true}).out);
    CHECK(j["diagnostics"][0]["severity"] == "error");
    CHECK(j["summary"]["errors"] == 1);
    CHECK(j["summary"]["warnings"] == 0);
}

TEST_CASE("json: no diagnostics — empty array, zero summary, still valid JSON") {
    const auto r = render({}, {.json = true});
    const auto j = nlohmann::json::parse(r.out);
    CHECK(j["diagnostics"].empty());
    CHECK(j["summary"]["warnings"] == 0);
    CHECK(j["summary"]["errors"] == 0);
}

TEST_CASE("json: output is exactly one JSON document, no human lines mixed in") {
    const auto r = render(mixed_diags(), {.json = true});
    CHECK(r.out.find("warning[") == std::string::npos);
    CHECK(r.out.back() == '\n');
    CHECK_NOTHROW(static_cast<void>(nlohmann::json::parse(r.out)));
}

// ── integration ───────────────────────────────────────────────────────────────

TEST_CASE("output: integration — three_slides.pptx, deck-level ctype line precedes slide lines") {
    std::string out;
    REQUIRE(run("three_slides.pptx", out) == 0);
    const auto l = lines(out);
    REQUIRE(l.size() >= 5);
    CHECK(l[0].find("[ctype]") != std::string::npos);
    CHECK(l[0].find(".pptx: warning") != std::string::npos);
    CHECK(l[1].find(".pptx:1: warning[empty-ph]") != std::string::npos);
    CHECK(l.back().find("4 warnings") != std::string::npos);
}

TEST_CASE("output: integration — three_slides.pptx --strict exits 2") {
    std::string out;
    CHECK(run("three_slides.pptx", out, {.strict = true}) == 2);
}

TEST_CASE("output: integration — three_slides.pptx --json parses with 4 diagnostics") {
    std::string out;
    REQUIRE(run("three_slides.pptx", out, {.json = true}) == 0);
    const auto j = nlohmann::json::parse(out);
    CHECK(j["schema_version"] == 1);
    CHECK(j["diagnostics"].size() == 4);
    CHECK(j["file"].get<std::string>().ends_with("three_slides.pptx"));
}

TEST_CASE("output: integration — unreadable file still exits 1 (distinct from --strict's 2)") {
    std::ostringstream oss;
    CHECK(pptctl::cmd::lint_to(fixture("does_not_exist.pptx"), oss, {.strict = true}) == 1);
}

// ── golden tests (#47) ────────────────────────────────────────────────────────
//
// Every fixture has its `lint --json` output checked in under tests/golden/ as
// <fixture>.lint.json. To regenerate after an intentional output change:
//   ./tests/golden/generate_golden.sh ./build/macos-release/pptctl

namespace {

std::string read_golden(const char* name) {
    const std::string path = std::string(PPTCTL_GOLDEN_DIR) + "/" + name;
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.is_open(), "golden file missing: ", path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Same canonicalisation as the extract goldens: the "file" field carries a
// machine-specific absolute path, so it is replaced by a placeholder.
std::string normalize_json_path(const std::string& json) {
    static const std::regex kFileField(R"("file": "[^"]*")");
    return std::regex_replace(json, kFileField, R"("file": "<path>")");
}

void check_lint_golden(const char* fixture_name, const char* golden_name) {
    std::string out;
    REQUIRE(run(fixture_name, out, {.json = true}) == 0);
    CHECK(normalize_json_path(out) == read_golden(golden_name));
}

// rule id -> number of diagnostics carrying it
std::map<std::string, int> rule_histogram(const nlohmann::json& doc) {
    std::map<std::string, int> h;
    for (const auto& d : doc["diagnostics"]) {
        ++h[d["rule"].get<std::string>()];
    }
    return h;
}

}  // namespace

TEST_CASE("golden: minimal.lint.json") {
    check_lint_golden("minimal.pptx", "minimal.lint.json");
}
TEST_CASE("golden: three_slides.lint.json") {
    check_lint_golden("three_slides.pptx", "three_slides.lint.json");
}
TEST_CASE("golden: images_deck.lint.json") {
    check_lint_golden("images_deck.pptx", "images_deck.lint.json");
}
TEST_CASE("golden: notes_and_masters.lint.json") {
    check_lint_golden("notes_and_masters.pptx", "notes_and_masters.lint.json");
}
TEST_CASE("golden: tables_deck.lint.json") {
    check_lint_golden("tables_deck.pptx", "tables_deck.lint.json");
}
TEST_CASE("golden: charts_deck.lint.json") {
    check_lint_golden("charts_deck.pptx", "charts_deck.lint.json");
}
TEST_CASE("golden: bloat_deck.lint.json") {
    check_lint_golden("bloat_deck.pptx", "bloat_deck.lint.json");
}
TEST_CASE("golden: lint_problems.lint.json") {
    check_lint_golden("lint_problems.pptx", "lint_problems.lint.json");
}
TEST_CASE("golden: lint_clean.lint.json") {
    check_lint_golden("lint_clean.pptx", "lint_clean.lint.json");
}

// The goldens above would happily freeze a regression; these pin the intent.

TEST_CASE("golden: lint_clean.pptx produces zero diagnostics") {
    std::string out;
    REQUIRE(run("lint_clean.pptx", out, {.json = true}) == 0);
    const auto j = nlohmann::json::parse(out);
    CHECK(j["diagnostics"].empty());
    CHECK(j["summary"]["warnings"] == 0);
    CHECK(j["summary"]["errors"] == 0);
}

TEST_CASE("golden: lint_clean.pptx --strict exits 0 and says so") {
    std::string out;
    CHECK(run("lint_clean.pptx", out, {.strict = true}) == 0);
    CHECK(out.find("no issues found") != std::string::npos);
}

TEST_CASE("golden: lint_problems.pptx trips every rule exactly once") {
    std::string out;
    REQUIRE(run("lint_problems.pptx", out, {.json = true}) == 0);
    const auto h = rule_histogram(nlohmann::json::parse(out));
    const std::map<std::string, int> want = {{"overflow", 1},
                                             {"tiny-text", 1},
                                             {"alt-text", 1},
                                             {"font-risk", 1},
                                             {"empty-ph", 1},
                                             {"ctype", 1},
                                             {"img-bloat", 1}};
    CHECK(h == want);
}

TEST_CASE("golden: lint_problems.pptx slide-level findings land on their own slide") {
    std::string out;
    REQUIRE(run("lint_problems.pptx", out, {.json = true}) == 0);
    const auto j = nlohmann::json::parse(out);
    std::map<std::string, int> slide_of;
    for (const auto& d : j["diagnostics"]) {
        if (!d["slide"].is_null()) {
            slide_of[d["rule"].get<std::string>()] = d["slide"].get<int>();
        }
    }
    CHECK(slide_of["overflow"] == 1);
    CHECK(slide_of["tiny-text"] == 2);
    CHECK(slide_of["alt-text"] == 3);
    CHECK(slide_of["empty-ph"] == 5);
    CHECK(slide_of["img-bloat"] == 7);
    // font-risk and ctype are deck-level (slide: null).
    CHECK(!slide_of.contains("font-risk"));
    CHECK(!slide_of.contains("ctype"));
}
