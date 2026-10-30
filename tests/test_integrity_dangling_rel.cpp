// Tests for the "dangling-rel" integrity rule (#129): an internal relationship
// whose target part doesn't exist.

#include <doctest/doctest.h>

#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>

#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::rel;
using integrity_test::rels;
using integrity_test::run_rule;

namespace {

constexpr const char* kRule = "dangling-rel";
constexpr const char* kSlideRels = "ppt/slides/_rels/slide1.xml.rels";
constexpr const char* kLayout = "../slideLayouts/slideLayout7.xml";

// minimal.pptx with slide 1's .rels replaced by its layout rel plus `extra`.
pptctl::PartChanges slide_rels_with(const std::string& extra) {
    pptctl::PartChanges changes;
    changes.replaced[kSlideRels] = rels({rel("rId1", "slideLayout", kLayout), extra});
    return changes;
}

}  // namespace

TEST_CASE("dangling-rel: the problems fixture has exactly its one injected defect") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "dangling-rel");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == kSlideRels);
    CHECK(d[0].slide_index == 0);
    CHECK(d[0].message ==
          R"(relationship rId4 (image) targets "ppt/media/missing.png", which does not exist)");
}

TEST_CASE("dangling-rel: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("dangling-rel: a NULL target, as some generators write, fires") {
    const EditedDeck deck("minimal.pptx", slide_rels_with(rel("rId2", "image", "NULL")));
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find(R"("ppt/slides/NULL")") != std::string::npos);
}

TEST_CASE("dangling-rel: an external relationship never fires") {
    const EditedDeck deck(
        "minimal.pptx",
        slide_rels_with(rel("rId2", "hyperlink", "https://example.com/gone.png", true)));
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("dangling-rel: the root .rels is checked too") {
    pptctl::Package src;
    REQUIRE(src.open(integrity_test::fixture("minimal.pptx")));
    std::string xml = integrity_test::to_string(src.read_part("_rels/.rels"));
    xml.replace(xml.find("docProps/app.xml"), 16, "docProps/gone.xml");
    pptctl::PartChanges changes;
    changes.replaced["_rels/.rels"] = integrity_test::to_bytes(xml);
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].part == "_rels/.rels");
}

TEST_CASE("dangling-rel: a .rels whose source part is missing is left to orphan-part") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide42.xml.rels"] =
        rels({rel("rId1", "image", "../media/nowhere.png")});
    const EditedDeck deck("minimal.pptx", changes);
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("dangling-rel: one finding per dangling relationship, in .rels then rId order") {
    const EditedDeck deck("minimal.pptx",
                          slide_rels_with(rel("rId2", "image", "../media/a.png") +
                                          rel("rId3", "image", "../media/b.png")));
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 2);
    CHECK(d[0].message.find("rId2") != std::string::npos);
    CHECK(d[1].message.find("rId3") != std::string::npos);
}
