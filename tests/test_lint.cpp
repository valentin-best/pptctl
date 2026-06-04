// Tests for the lint subcommand.

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <pptctl/lint.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
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
