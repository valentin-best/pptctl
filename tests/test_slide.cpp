// Tests for the Slide content model (shape tree, text frames, paragraphs, runs).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

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

// Bulleted paragraphs at two levels, an empty paragraph, a field, and a break.
constexpr const char* kRichParagraphs = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Body 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody>
        <a:bodyPr/>
        <a:p><a:r><a:t>Top</a:t></a:r></a:p>
        <a:p><a:pPr lvl="1"/><a:r><a:t>Nested</a:t></a:r></a:p>
        <a:p/>
        <a:p>
          <a:r><a:t>Line one</a:t></a:r>
          <a:br/>
          <a:fld id="{GUID}" type="slidenum"><a:t>3</a:t></a:fld>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";

// One placeholder (with type + xfrm), a picture, a table, a chart, and a
// connector — to exercise kind detection, placeholder type, and geometry.
constexpr const char* kMixedShapes = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Title 1"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="ctrTitle"/></p:nvPr></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="685800" y="2130425"/><a:ext cx="7772400" cy="2898775"/></a:xfrm></p:spPr>
      <p:txBody><a:bodyPr/><a:p><a:r><a:t>Title</a:t></a:r></a:p></p:txBody>
    </p:sp>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="3" name="Picture 3"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill/><p:spPr/>
    </p:pic>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="4" name="Table 4"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"/></a:graphic>
    </p:graphicFrame>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="5" name="Chart 5"/></p:nvGraphicFramePr>
      <a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart"/></a:graphic>
    </p:graphicFrame>
    <p:cxnSp>
      <p:nvCxnSpPr><p:cNvPr id="6" name="Connector 6"/></p:nvCxnSpPr>
      <p:spPr/>
    </p:cxnSp>
  </p:spTree></p:cSld>
</p:sld>)";

// A picture whose blip is embedded (internal), with alt text on cNvPr@descr.
constexpr const char* kEmbeddedPicture = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="2" name="Picture 2" descr="A red square"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:embed="rId2"/></p:blipFill>
      <p:spPr/>
    </p:pic>
  </p:spTree></p:cSld>
</p:sld>)";

// A picture whose blip is a link to an external image.
constexpr const char* kLinkedPicture = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:pic>
      <p:nvPicPr><p:cNvPr id="2" name="Linked"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
      <p:blipFill><a:blip r:link="rId5"/></p:blipFill>
      <p:spPr/>
    </p:pic>
  </p:spTree></p:cSld>
</p:sld>)";

// A top-level shape, then a group containing two shapes — to check recursion
// and that document order is preserved at both levels.
constexpr const char* kGroupedShapes = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Outer"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:p><a:r><a:t>Outer</a:t></a:r></a:p></p:txBody>
    </p:sp>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="3" name="Group 3"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr/>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="4" name="Inner A"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/><a:p><a:r><a:t>A</a:t></a:r></a:p></p:txBody>
      </p:sp>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="5" name="Inner B"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr/>
        <p:txBody><a:bodyPr/><a:p><a:r><a:t>B</a:t></a:r></a:p></p:txBody>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";

// A picture nested inside a group shape — exercises picture capture through the
// group recursion (the same recursion resolve_pictures walks in load()).
constexpr const char* kGroupedPicture = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 2"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr/>
      <p:pic>
        <p:nvPicPr><p:cNvPr id="3" name="Inner Pic" descr="nested"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
        <p:blipFill><a:blip r:embed="rId7"/></p:blipFill>
        <p:spPr/>
      </p:pic>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";

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

// A notesSlide with the standard placeholder set: a slide-image placeholder,
// the notes-body placeholder (the only one with text), and a slide-number
// placeholder. Only the body's text should surface as notes.
constexpr const char* kNotesSlide = R"(<?xml version="1.0"?>
<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
         xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Slide Image Placeholder 1"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="sldImg" idx="2"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="3" name="Notes Placeholder 2"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="body" idx="3"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:lstStyle/>
        <a:p><a:r><a:t>First note line.</a:t></a:r></a:p>
        <a:p><a:r><a:t>Second note line.</a:t></a:r></a:p>
      </p:txBody>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="4" name="Slide Number Placeholder 3"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="sldNum" idx="5"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:p><a:fld id="{X}" type="slidenum"><a:t>1</a:t></a:fld></a:p></p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:notes>)";

// A notesSlide carrying only a slide-image placeholder — no notes body.
constexpr const char* kNotesSlideNoBody = R"(<?xml version="1.0"?>
<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
         xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Slide Image Placeholder 1"/><p:cNvSpPr/>
        <p:nvPr><p:ph type="sldImg" idx="2"/></p:nvPr></p:nvSpPr>
      <p:spPr/>
    </p:sp>
  </p:spTree></p:cSld>
</p:notes>)";

// A slide with a single chart graphicFrame that carries a c:chart rel reference.
constexpr const char* kChartSlide = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:graphicFrame>
      <p:nvGraphicFramePr><p:cNvPr id="2" name="Chart 2"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>
      <p:xfrm><a:off x="914400" y="914400"/><a:ext cx="5486400" cy="3657600"/></p:xfrm>
      <a:graphic>
        <a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">
          <c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rId2"/>
        </a:graphicData>
      </a:graphic>
    </p:graphicFrame>
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

TEST_CASE("Slide: parses levels, empty paragraphs, fields, and breaks") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kRichParagraphs));
    REQUIRE(slide.shapes().size() == 1);
    const auto& paras = slide.shapes()[0].paragraphs;
    REQUIRE(paras.size() == 4);

    CHECK(paras[0].level == 0);
    CHECK(paras[0].text() == "Top");

    CHECK(paras[1].level == 1);
    CHECK(paras[1].text() == "Nested");

    // Empty paragraph preserved as structure.
    CHECK(paras[2].runs.empty());
    CHECK(paras[2].text().empty());

    // Run, break (newline), then a field run — in document order.
    REQUIRE(paras[3].runs.size() == 3);
    CHECK(paras[3].runs[0].text == "Line one");
    CHECK_FALSE(paras[3].runs[0].is_field);
    CHECK(paras[3].runs[1].text == "\n");
    CHECK(paras[3].runs[2].text == "3");
    CHECK(paras[3].runs[2].is_field);
}

TEST_CASE("Slide: detects shape kinds, placeholder type, and geometry") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kMixedShapes));
    REQUIRE(slide.shapes().size() == 5);
    const auto& s = slide.shapes();

    CHECK(s[0].kind == pptctl::ShapeKind::Placeholder);
    REQUIRE(s[0].placeholder_type.has_value());
    CHECK(*s[0].placeholder_type == "ctrTitle");
    REQUIRE(s[0].geometry.has_value());
    CHECK(s[0].geometry->off_x == 685800);
    CHECK(s[0].geometry->ext_cy == 2898775);

    CHECK(s[1].kind == pptctl::ShapeKind::Picture);
    CHECK(s[1].name == "Picture 3");
    CHECK_FALSE(s[1].geometry.has_value());  // no a:xfrm

    CHECK(s[2].kind == pptctl::ShapeKind::Table);
    CHECK(s[3].kind == pptctl::ShapeKind::Chart);
    CHECK(s[4].kind == pptctl::ShapeKind::Connector);

    // A chart graphicFrame with no c:chart child yields no chart model.
    CHECK_FALSE(s[3].table.has_value());
    CHECK_FALSE(s[3].chart.has_value());
    // The table graphicFrame in this fixture has no a:tbl child either.
    CHECK_FALSE(s[2].table.has_value());
}

TEST_CASE("Slide: recurses into group shapes preserving reading order") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kGroupedShapes));
    REQUIRE(slide.shapes().size() == 2);

    CHECK(slide.shapes()[0].name == "Outer");

    const auto& group = slide.shapes()[1];
    CHECK(group.kind == pptctl::ShapeKind::Group);
    CHECK(group.name == "Group 3");
    REQUIRE(group.children.size() == 2);
    CHECK(group.children[0].name == "Inner A");
    CHECK(group.children[0].paragraphs.at(0).text() == "A");
    CHECK(group.children[1].name == "Inner B");
    CHECK(group.children[1].paragraphs.at(0).text() == "B");
}

TEST_CASE("Presentation: slide_parts returns slide part names in order") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    const auto& parts = pres.slide_parts();
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "ppt/slides/slide1.xml");
    CHECK(parts[1] == "ppt/slides/slide2.xml");
    CHECK(parts[2] == "ppt/slides/slide3.xml");
    CHECK(parts.size() == pres.slide_count());
}

TEST_CASE("Slide: loads slide1 text from three_slides.pptx") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));
    CHECK(slide.part_name() == "ppt/slides/slide1.xml");

    // The first slide's title placeholder reads "Slide 1".
    bool found_title = false;
    for (const auto& s : slide.shapes()) {
        for (const auto& p : s.paragraphs) {
            if (p.text() == "Slide 1") {
                found_title = true;
            }
        }
    }
    CHECK(found_title);
}

TEST_CASE("Slide: images_deck slides carry picture shapes") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    REQUIRE_FALSE(pres.slide_parts().empty());

    bool any_picture = false;
    for (const auto& part : pres.slide_parts()) {
        pptctl::Slide slide;
        REQUIRE(slide.load(pkg, part));
        for (const auto& s : slide.shapes()) {
            if (s.kind == pptctl::ShapeKind::Picture) {
                any_picture = true;
            }
        }
    }
    CHECK(any_picture);
}

TEST_CASE("Slide: captures an embedded picture's blip reference and alt text") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kEmbeddedPicture));
    REQUIRE(slide.shapes().size() == 1);

    const auto& pic = slide.shapes()[0];
    CHECK(pic.kind == pptctl::ShapeKind::Picture);
    REQUIRE(pic.picture.has_value());
    CHECK(pic.picture->rel_id == "rId2");
    CHECK_FALSE(pic.picture->linked);
    REQUIRE(pic.picture->alt_text.has_value());
    CHECK(*pic.picture->alt_text == "A red square");
    // parse() does not resolve — that is load()'s job.
    CHECK(pic.picture->target.empty());
    CHECK_FALSE(pic.picture->size_bytes.has_value());
}

TEST_CASE("Slide: flags a linked (external) picture") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kLinkedPicture));
    REQUIRE(slide.shapes().size() == 1);

    const auto& pic = slide.shapes()[0];
    REQUIRE(pic.picture.has_value());
    CHECK(pic.picture->linked);
    CHECK(pic.picture->rel_id == "rId5");
    CHECK_FALSE(pic.picture->alt_text.has_value());
}

TEST_CASE("Slide: resolves an embedded picture to its media part on load") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));

    const pptctl::Picture* pic = nullptr;
    for (const auto& s : slide.shapes()) {
        if (s.kind == pptctl::ShapeKind::Picture && s.picture.has_value()) {
            pic = &*s.picture;
            break;  // slide 1 has a single picture; take the first
        }
    }
    REQUIRE(pic != nullptr);
    // Expected values come from the generated fixture (tests/fixtures/
    // generate_fixtures.py): rId2 -> ppt/media/image1.png, a 120-byte PNG.
    CHECK(pic->target == "ppt/media/image1.png");
    CHECK(pic->content_type == "image/png");
    REQUIRE(pic->size_bytes.has_value());
    CHECK(*pic->size_bytes == 120);
}

TEST_CASE("Slide: captures a picture nested inside a group") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kGroupedPicture));
    REQUIRE(slide.shapes().size() == 1);

    const auto& group = slide.shapes()[0];
    CHECK(group.kind == pptctl::ShapeKind::Group);
    REQUIRE(group.children.size() == 1);

    const auto& pic = group.children[0];
    CHECK(pic.kind == pptctl::ShapeKind::Picture);
    REQUIRE(pic.picture.has_value());
    CHECK(pic.picture->rel_id == "rId7");
    CHECK_FALSE(pic.picture->linked);
    REQUIRE(pic.picture->alt_text.has_value());
    CHECK(*pic.picture->alt_text == "nested");
}

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

TEST_CASE("Slide: default-constructed slide has no notes") {
    pptctl::Slide slide;
    CHECK(slide.notes().empty());
    CHECK(slide.notes_text().empty());
}

TEST_CASE("Slide: parse_notes of malformed XML returns false") {
    pptctl::Slide slide;
    CHECK_FALSE(slide.parse_notes("<not-closed"));
}

TEST_CASE("Slide: parse_notes captures only the notes-body placeholder text") {
    pptctl::Slide slide;
    REQUIRE(slide.parse_notes(kNotesSlide));
    REQUIRE(slide.notes().size() == 2);
    CHECK(slide.notes()[0].text() == "First note line.");
    CHECK(slide.notes()[1].text() == "Second note line.");
    CHECK(slide.notes_text() == "First note line.\nSecond note line.");
}

TEST_CASE("Slide: parse_notes with no body placeholder yields empty notes") {
    pptctl::Slide slide;
    REQUIRE(slide.parse_notes(kNotesSlideNoBody));
    CHECK(slide.notes().empty());
    CHECK(slide.notes_text().empty());
}

TEST_CASE("Slide: load resolves speaker notes via the notesSlide relationship") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    REQUIRE(pres.slide_parts().size() == 4);

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));
    // Notes text from the generated fixture (generate_fixtures.py).
    CHECK(slide.notes_text() == "Speaker notes for slide 1: remember to elaborate here.");
}

TEST_CASE("Slide: a slide without notes loads with empty notes") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    pptctl::Slide slide;
    REQUIRE(slide.load(pkg, pres.slide_parts().at(0)));
    CHECK(slide.notes().empty());
}

TEST_CASE("Slide: parse captures chart rel_id from c:chart element") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kChartSlide));
    REQUIRE(slide.shapes().size() == 1);

    const auto& shape = slide.shapes()[0];
    CHECK(shape.kind == pptctl::ShapeKind::Chart);
    CHECK(shape.name == "Chart 2");
    REQUIRE(shape.chart.has_value());
    CHECK(shape.chart->rel_id == "rId2");
    // parse() does not resolve — target and chart_type are empty until load().
    CHECK(shape.chart->target.empty());
    CHECK(shape.chart->chart_type.empty());
    CHECK(shape.chart->series.empty());
}

TEST_CASE("Slide: loads charts from charts_deck.pptx") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("charts_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    REQUIRE(pres.slide_parts().size() == 2);

    auto first_chart = [](const pptctl::Slide& slide) -> const pptctl::Chart* {
        for (const auto& s : slide.shapes()) {
            if (s.kind == pptctl::ShapeKind::Chart && s.chart.has_value()) {
                return &*s.chart;
            }
        }
        return nullptr;
    };

    // Slide 1: clustered-bar chart, 2 series x 3 categories.
    pptctl::Slide s1;
    REQUIRE(s1.load(pkg, pres.slide_parts().at(0)));
    const pptctl::Chart* c1 = first_chart(s1);
    REQUIRE(c1 != nullptr);
    CHECK(c1->chart_type == "bar");
    REQUIRE(c1->title.has_value());
    CHECK(*c1->title == "Revenue vs Cost");
    REQUIRE(c1->series.size() == 2);

    const auto& rev = c1->series[0];
    CHECK(rev.name == "Revenue");
    REQUIRE(rev.categories.size() == 3);
    CHECK(rev.categories[0] == "Q1");
    CHECK(rev.categories[1] == "Q2");
    CHECK(rev.categories[2] == "Q3");
    REQUIRE(rev.values.size() == 3);
    CHECK(rev.values[0] == 100.0);
    CHECK(rev.values[1] == 200.0);
    CHECK(rev.values[2] == 150.0);

    const auto& cost = c1->series[1];
    CHECK(cost.name == "Cost");
    REQUIRE(cost.values.size() == 3);
    CHECK(cost.values[0] == 80.0);
    CHECK(cost.values[1] == 120.0);
    CHECK(cost.values[2] == 90.0);

    // Slide 2: pie chart, 1 series x 3 categories.
    pptctl::Slide s2;
    REQUIRE(s2.load(pkg, pres.slide_parts().at(1)));
    const pptctl::Chart* c2 = first_chart(s2);
    REQUIRE(c2 != nullptr);
    CHECK(c2->chart_type == "pie");
    REQUIRE(c2->title.has_value());
    CHECK(*c2->title == "Market Share");
    REQUIRE(c2->series.size() == 1);

    const auto& share = c2->series[0];
    CHECK(share.name == "Share");
    REQUIRE(share.categories.size() == 3);
    CHECK(share.categories[0] == "Alpha");
    REQUIRE(share.values.size() == 3);
    CHECK(share.values[0] == 30.0);
    CHECK(share.values[1] == 50.0);
    CHECK(share.values[2] == 20.0);
}

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

// ── group child space (#59) ───────────────────────────────────────────────────

TEST_CASE("Slide: group geometry captures chOff/chExt as child_space") {
    const char* xml = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm flipH="1">
        <a:off x="435427" y="5043713"/><a:ext cx="8305815" cy="1651001"/>
        <a:chOff x="98612" y="5007574"/><a:chExt cx="6346007" cy="1926789"/>
      </a:xfrm></p:grpSpPr>
      <p:sp>
        <p:nvSpPr><p:cNvPr id="3" name="Child"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
        <p:spPr><a:xfrm><a:off x="98612" y="5007574"/><a:ext cx="6346007" cy="1926789"/></a:xfrm></p:spPr>
      </p:sp>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    REQUIRE(slide.shapes().size() == 1);
    const auto& grp = slide.shapes()[0];
    REQUIRE(grp.geometry.has_value());
    CHECK(grp.geometry->off_x == 435427);
    CHECK(grp.geometry->ext_cx == 8305815);
    REQUIRE(grp.geometry->child_space.has_value());
    CHECK(grp.geometry->child_space->off_x == 98612);
    CHECK(grp.geometry->child_space->off_y == 5007574);
    CHECK(grp.geometry->child_space->ext_cx == 6346007);
    CHECK(grp.geometry->child_space->ext_cy == 1926789);

    REQUIRE(grp.children.size() == 1);
    REQUIRE(grp.children[0].geometry.has_value());
    CHECK_FALSE(grp.children[0].geometry->child_space.has_value());
}

TEST_CASE("Slide: group xfrm without chOff/chExt leaves child_space empty") {
    const char* xml = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:grpSp>
      <p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="100" cy="100"/></a:xfrm></p:grpSpPr>
    </p:grpSp>
  </p:spTree></p:cSld>
</p:sld>)";
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    REQUIRE(slide.shapes().size() == 1);
    REQUIRE(slide.shapes()[0].geometry.has_value());
    CHECK_FALSE(slide.shapes()[0].geometry->child_space.has_value());
}

TEST_CASE("Slide: load hashes each embedded picture's media bytes and probes its size") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());

    std::vector<std::uint64_t> hashes;
    for (const auto& part : pres.slide_parts()) {
        pptctl::Slide slide;
        REQUIRE(slide.load(pkg, part));
        for (const auto& s : slide.shapes()) {
            if (!s.picture) {
                continue;
            }
            REQUIRE(s.picture->media_hash.has_value());
            REQUIRE(s.picture->dimensions.has_value());
            CHECK(s.picture->dimensions->width == 64 + hashes.size());
            CHECK(s.picture->dimensions->height == 48 + hashes.size());
            // The hash is the FNV-1a 64 of the part bytes, so it can be
            // recomputed from the package alone.
            const auto bytes = pkg.read_part(s.picture->target);
            CHECK(*s.picture->media_hash == pptctl::fnv1a64(bytes));
            hashes.push_back(*s.picture->media_hash);
        }
    }
    // 12 distinct PNGs -> 12 distinct hashes.
    REQUIRE(hashes.size() == 12);
    std::sort(hashes.begin(), hashes.end());
    CHECK(std::unique(hashes.begin(), hashes.end()) == hashes.end());
}

TEST_CASE("Slide: parse alone leaves media_hash and dimensions unset") {
    pptctl::Slide slide;
    REQUIRE(slide.parse(kEmbeddedPicture));
    REQUIRE(slide.shapes().at(0).picture.has_value());
    CHECK_FALSE(slide.shapes()[0].picture->media_hash.has_value());
    CHECK_FALSE(slide.shapes()[0].picture->dimensions.has_value());
}

// ── solid fills (#72) ─────────────────────────────────────────────────────────

TEST_CASE("Slide: parses solid fills (srgb, scheme, none) and leaves inherited fills empty") {
    const char* xml = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Srgb"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1" cy="1"/></a:xfrm>
        <a:solidFill><a:srgbClr val="ff8800"/></a:solidFill></p:spPr>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="3" name="Scheme"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:solidFill><a:schemeClr val="accent1"><a:lumMod val="75000"/></a:schemeClr></a:solidFill></p:spPr>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="4" name="None"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:noFill/></p:spPr>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="5" name="Inherited"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
    </p:sp>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="6" name="Gradient"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="000000"/></a:gs></a:gsLst></a:gradFill></p:spPr>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    REQUIRE(slide.shapes().size() == 5);

    const auto& srgb = slide.shapes()[0];
    REQUIRE(srgb.fill.has_value());
    CHECK(srgb.fill->kind == "solid");
    CHECK(srgb.fill->color == "FF8800");  // hex is normalized to upper case

    const auto& scheme = slide.shapes()[1];
    REQUIRE(scheme.fill.has_value());
    CHECK(scheme.fill->kind == "solid");
    CHECK(scheme.fill->color == "accent1");

    const auto& none = slide.shapes()[2];
    REQUIRE(none.fill.has_value());
    CHECK(none.fill->kind == "none");
    CHECK(none.fill->color.empty());

    CHECK_FALSE(slide.shapes()[3].fill.has_value());
    CHECK_FALSE(slide.shapes()[4].fill.has_value());  // gradients are not modelled
}

// ── whitespace-only runs (#77 finding) ────────────────────────────────────────
//
// PowerPoint routinely stores a space between two differently formatted words
// as its own run (`<a:t> </a:t>`). pugixml skips whitespace-only text nodes
// unless told otherwise, which silently glued the words together.

TEST_CASE("Slide: a run whose text is a single space is kept") {
    const char* xml = R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name="Body"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr/>
      <p:txBody><a:bodyPr/><a:lstStyle/>
        <a:p>
          <a:r><a:rPr b="1"/><a:t>www.google.com</a:t></a:r>
          <a:r><a:t> </a:t></a:r>
          <a:r><a:t>= 74.125.131.26</a:t></a:r>
        </a:p>
      </p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
    pptctl::Slide slide;
    REQUIRE(slide.parse(xml));
    REQUIRE(slide.shapes().size() == 1);
    REQUIRE(slide.shapes()[0].paragraphs.size() == 1);
    const auto& runs = slide.shapes()[0].paragraphs[0].runs;
    REQUIRE(runs.size() == 3);
    CHECK(runs[1].text == " ");
    CHECK(slide.shapes()[0].paragraphs[0].text() == "www.google.com = 74.125.131.26");
}
