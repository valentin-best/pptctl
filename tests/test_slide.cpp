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
