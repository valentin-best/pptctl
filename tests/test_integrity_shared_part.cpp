// Tests for the "shared-part" integrity rule (#133): a part that must have a
// single owner (notes, tags, charts and their style parts, ...) referenced by
// several. Cf. iOfficeAI/OfficeCLI#540, #541.

#include <doctest/doctest.h>

#include <pptctl/integrity.hpp>
#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>
#include <string_view>

#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::rel;
using integrity_test::rels;
using integrity_test::run_rule;

namespace {

constexpr const char* kRule = "shared-part";
constexpr const char* kSlide = "ppt/slides/slide1.xml";
constexpr const char* kSlideRels = "ppt/slides/_rels/slide1.xml.rels";
constexpr const char* kLayout = "../slideLayouts/slideLayout7.xml";

// A slide with one text box per entry of `tag_rids`, each carrying
// <p:tags r:id="..."/> as PowerPoint add-ins write them.
std::string tagged_slide(const std::vector<std::string>& tag_rids) {
    std::string shapes;
    int id = 2;
    for (const auto& rid : tag_rids) {
        shapes += R"(<p:sp><p:nvSpPr><p:cNvPr id=")" + std::to_string(id++) +
                  R"(" name="Box"/><p:cNvSpPr txBox="1"/><p:nvPr><p:custDataLst><p:tags r:id=")" +
                  rid + R"("/></p:custDataLst></p:nvPr></p:nvSpPr><p:spPr/></p:sp>)";
    }
    return R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
           R"(<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
           R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" )"
           R"(xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
           R"(<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>)"
           R"(</p:nvGrpSpPr><p:grpSpPr/>)" +
           shapes + "</p:spTree></p:cSld></p:sld>";
}

pptctl::PartChanges tagged(const std::vector<std::string>& tag_rids,
                           const std::vector<std::string>& extra_rels) {
    pptctl::PartChanges changes;
    changes.replaced[kSlide] = integrity_test::to_bytes(tagged_slide(tag_rids));
    std::vector<std::string> entries{rel("rId1", "slideLayout", kLayout)};
    entries.insert(entries.end(), extra_rels.begin(), extra_rels.end());
    changes.replaced[kSlideRels] = rels(entries);
    changes.replaced["ppt/tags/tag1.xml"] = integrity_test::to_bytes("<p:tagLst/>");
    return changes;
}

}  // namespace

TEST_CASE("shared-part: the problems fixture has its two injected defects") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 2);
    CHECK(d[0].rule_id == "shared-part");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == "ppt/charts/style1.xml");
    CHECK(d[0].message ==
          R"(chartStyle part "ppt/charts/style1.xml" is shared by ppt/charts/chart1.xml, )"
          "ppt/charts/chart2.xml; each needs its own copy");
    CHECK(d[1].part == "ppt/notesSlides/notesSlide1.xml");
    CHECK(d[1].message.find("ppt/slides/slide1.xml, ppt/slides/slide2.xml") != std::string::npos);
}

TEST_CASE("shared-part: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("shared-part: two shapes on one slide using the same tags rId fire (OfficeCLI#540)") {
    const EditedDeck deck("minimal.pptx",
                          tagged({"rId2", "rId2"}, {rel("rId2", "tags", "../tags/tag1.xml")}));
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].part == "ppt/tags/tag1.xml");
    CHECK(d[0].message ==
          R"(tags part "ppt/tags/tag1.xml" is shared by ppt/slides/slide1.xml (2 references); )"
          "each needs its own copy");
}

TEST_CASE("shared-part: two rIds to the same tags part, each used once, fire") {
    const EditedDeck deck(
        "minimal.pptx",
        tagged({"rId2", "rId3"},
               {rel("rId2", "tags", "../tags/tag1.xml"), rel("rId3", "tags", "../tags/tag1.xml")}));
    CHECK(run_rule(kRule, deck.pkg).size() == 1);
}

TEST_CASE("shared-part: one tags part used once is fine") {
    const EditedDeck deck("minimal.pptx",
                          tagged({"rId2"}, {rel("rId2", "tags", "../tags/tag1.xml")}));
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("shared-part: a duplicate implicit rel from one owner doesn't count") {
    // Two notesSlide rels from the same slide to the same notes: odd, harmless.
    const EditedDeck deck("minimal.pptx",
                          tagged({},
                                 {rel("rId2", "notesSlide", "../notesSlides/notesSlide1.xml"),
                                  rel("rId3", "notesSlide", "../notesSlides/notesSlide1.xml")}));
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("shared-part: shareable types — media, layouts, themes — never fire") {
    CHECK_FALSE(pptctl::is_exclusively_owned(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"));
    CHECK_FALSE(pptctl::is_exclusively_owned(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout"));
    CHECK_FALSE(pptctl::is_exclusively_owned(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme"));
    CHECK(pptctl::is_exclusively_owned(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide"));
    CHECK(pptctl::is_exclusively_owned(
        "http://schemas.microsoft.com/office/2011/relationships/chartColorStyle"));
}
