// Tests for the Slide content model (shape tree, text frames, paragraphs, runs).

#include <doctest/doctest.h>

#include <pptctl/slide.hpp>
#include <string>

namespace {

// A minimal slide with an empty shape tree.
constexpr const char* kEmptySlide = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
  </p:spTree></p:cSld>
</p:sld>)";

// One placeholder shape with a two-run title paragraph.
constexpr const char* kOneTextShape = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Title 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/><a:lstStyle/>
        <a:p>
          <a:r><a:t>Hello </a:t></a:r>
          <a:r><a:t>World</a:t></a:r>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";

// A run with explicit formatting, plus a bare run (no rPr) to check "unset".
constexpr const char* kFormattedRuns = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="TextBox 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/>
        <a:p>
          <a:r>
            <a:rPr b="1" i="0" u="sng" sz="2400"><a:latin typeface="Arial"/></a:rPr>
            <a:t>Bold</a:t>
          </a:r>
          <a:r><a:t>Plain</a:t></a:r>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";

}  // namespace

TEST_CASE("Slide: default-constructed slide has no shapes") {
    pptctl::Slide slide;
    CHECK(slide.shapes().empty());
    CHECK(slide.index() == 0);
}

TEST_CASE("Slide: parse of malformed XML returns false") {
    pptctl::Slide slide;
    CHECK_FALSE(slide.parse("<not-closed"));
}

TEST_CASE("Slide: parse of an empty shape tree yields no shapes") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kEmptySlide));
    CHECK(slide.shapes().empty());
}

TEST_CASE("Slide: parses a single text shape into paragraphs and runs") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kOneTextShape));
    REQUIRE(slide.shapes().size() == 1);

    const auto& sp = slide.shapes()[0];
    CHECK(sp.id == "2");
    CHECK(sp.name == "Title 1");
    REQUIRE(sp.paragraphs.size() == 1);
    REQUIRE(sp.paragraphs[0].runs.size() == 2);
    CHECK(sp.paragraphs[0].runs[0].text == "Hello ");
    CHECK(sp.paragraphs[0].runs[1].text == "World");
    CHECK(sp.paragraphs[0].text() == "Hello World");
}

TEST_CASE("Slide: parses run properties and leaves unset fields empty") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kFormattedRuns));
    REQUIRE(slide.shapes().size() == 1);
    const auto& runs = slide.shapes()[0].paragraphs.at(0).runs;
    REQUIRE(runs.size() == 2);

    const auto& fmt = runs[0].props;
    REQUIRE(fmt.bold.has_value());
    CHECK(*fmt.bold == true);
    REQUIRE(fmt.italic.has_value());
    CHECK(*fmt.italic == false);
    REQUIRE(fmt.underlined.has_value());
    CHECK(*fmt.underlined == true);
    REQUIRE(fmt.size_centipt.has_value());
    CHECK(*fmt.size_centipt == 2400);
    CHECK(fmt.size_pt() == doctest::Approx(24.0));
    REQUIRE(fmt.font.has_value());
    CHECK(*fmt.font == "Arial");

    // Bare run: every property unset.
    const auto& bare = runs[1].props;
    CHECK_FALSE(bare.bold.has_value());
    CHECK_FALSE(bare.italic.has_value());
    CHECK_FALSE(bare.underlined.has_value());
    CHECK_FALSE(bare.size_centipt.has_value());
    CHECK_FALSE(bare.font.has_value());
}
