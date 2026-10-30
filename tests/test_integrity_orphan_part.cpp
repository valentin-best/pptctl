// Tests for the "orphan-part" integrity rule (#132): a part no relationship
// reaches.

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

constexpr const char* kRule = "orphan-part";

}  // namespace

TEST_CASE("orphan-part: the problems fixture has exactly its one injected defect") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "orphan-part");
    CHECK(d[0].severity == pptctl::Severity::Warning);
    CHECK(d[0].part == "ppt/media/orphan.png");
    CHECK(d[0].slide_index == 0);
    CHECK(d[0].message == R"(part "ppt/media/orphan.png" is not reachable from any relationship; )"
                          "`pptctl optimize` removes orphan media");
}

TEST_CASE("orphan-part: every clean fixture is silent, bar optimize_deck's deliberate orphan") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        const auto d = run_rule(kRule, path);
        if (integrity_test::fs::path(path).filename() == "optimize_deck.pptx") {
            // Kept for the optimize orphans pass (#99).
            REQUIRE(d.size() == 1);
            CHECK(d[0].part == "ppt/media/orphan.png");
        } else {
            CHECK(d.empty());
        }
    }
}

TEST_CASE("orphan-part: an unreachable part's .rels is folded into its finding") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide42.xml"] = integrity_test::to_bytes("<p:sld/>");
    changes.replaced["ppt/slides/_rels/slide42.xml.rels"] =
        rels({rel("rId1", "slideLayout", "../slideLayouts/slideLayout7.xml")});
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].part == "ppt/slides/slide42.xml");
    CHECK(
        d[0].message ==
        R"(part "ppt/slides/slide42.xml" (and its .rels) is not reachable from any relationship)");
}

TEST_CASE("orphan-part: a .rels whose source part doesn't exist is reported itself") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide42.xml.rels"] =
        rels({rel("rId1", "image", "../media/nowhere.png")});
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].part == "ppt/slides/_rels/slide42.xml.rels");
    CHECK(d[0].message == R"(relationships part "ppt/slides/_rels/slide42.xml.rels" belongs to )"
                          R"("ppt/slides/slide42.xml", which does not exist)");
}

TEST_CASE("orphan-part: only media gets the optimize hint") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/tags/tag9.xml"] = integrity_test::to_bytes("<tagLst/>");
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message == R"(part "ppt/tags/tag9.xml" is not reachable from any relationship)");
}
