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
