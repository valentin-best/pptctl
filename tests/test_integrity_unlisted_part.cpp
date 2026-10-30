// Tests for the "unlisted-part" integrity rule (#152): a slide or master the
// presentation part relates but doesn't list.

#include <doctest/doctest.h>

#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>

#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::rel;
using integrity_test::run_rule;
using integrity_test::to_bytes;
using integrity_test::to_string;

namespace {

constexpr const char* kRule = "unlisted-part";

// `fixture_name`'s presentation.xml with `from` replaced by `to` (once).
pptctl::PartChanges edit_presentation(std::string_view fixture_name,
                                      std::string_view from,
                                      std::string_view to) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(integrity_test::fixture(fixture_name)));
    auto xml = to_string(pkg.read_part("ppt/presentation.xml"));
    const auto at = xml.find(from);
    REQUIRE(at != std::string::npos);
    xml.replace(at, from.size(), to);
    pptctl::PartChanges changes;
    changes.replaced["ppt/presentation.xml"] = to_bytes(xml);
    return changes;
}

}  // namespace

TEST_CASE("unlisted-part: the problems fixture has exactly its one injected defect") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "unlisted-part");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == "ppt/presentation.xml");
    CHECK(d[0].slide_index == 0);
    CHECK(d[0].message.starts_with("relationship rId"));
    CHECK(d[0].message.ends_with(
        R"( (slide) targets "ppt/slides/slide9.xml", but ppt/presentation.xml doesn't list it in )"
        "p:sldIdLst: it is not part of the slide show"));
}

TEST_CASE("unlisted-part: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("unlisted-part: a notes master that is related but not listed (python-pptx output)") {
    const auto changes = edit_presentation("notes_and_masters.pptx",
                                           R"(<p:notesMasterIdLst><p:notesMasterId r:id=")",
                                           R"(<p:notesMasterIdLst><p:unknown r:id=")");
    const EditedDeck deck("notes_and_masters.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find(R"((notesMaster) targets "ppt/notesMasters/notesMaster1.xml")") !=
          std::string::npos);
    CHECK(d[0].message.ends_with(
        "doesn't list it in p:notesMasterIdLst: Keynote refuses to open the deck"));
}

TEST_CASE("unlisted-part: a slide master left out of p:sldMasterIdLst") {
    // notes_and_masters has two masters; drop the second one's entry.
    const auto changes =
        edit_presentation("notes_and_masters.pptx", R"(<p:sldMasterId id="2147483700")", R"(<p:x)");
    const EditedDeck deck("notes_and_masters.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find(R"((slideMaster) targets "ppt/slideMasters/slideMaster2.xml")") !=
          std::string::npos);
    CHECK(d[0].message.ends_with("doesn't list it in p:sldMasterIdLst"));
}

TEST_CASE("unlisted-part: a related handout master with no p:handoutMasterIdLst") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(integrity_test::fixture("minimal.pptx")));
    auto pres_rels = to_string(pkg.read_part("ppt/_rels/presentation.xml.rels"));
    pres_rels.insert(pres_rels.find("</Relationships>"),
                     rel("rId900", "handoutMaster", "handoutMasters/handoutMaster1.xml"));
    pptctl::PartChanges changes;
    changes.replaced["ppt/_rels/presentation.xml.rels"] = to_bytes(pres_rels);
    changes.replaced["ppt/handoutMasters/handoutMaster1.xml"] = to_bytes("<p:handoutMaster/>");
    const EditedDeck deck("minimal.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message ==
          R"(relationship rId900 (handoutMaster) targets "ppt/handoutMasters/handoutMaster1.xml", )"
          "but ppt/presentation.xml doesn't list it in p:handoutMasterIdLst");
}

TEST_CASE("unlisted-part: a missing target is dangling-rel's, not this rule's") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(integrity_test::fixture("minimal.pptx")));
    auto pres_rels = to_string(pkg.read_part("ppt/_rels/presentation.xml.rels"));
    pres_rels.insert(pres_rels.find("</Relationships>"),
                     rel("rId901", "slide", "slides/slide404.xml"));
    pptctl::PartChanges changes;
    changes.replaced["ppt/_rels/presentation.xml.rels"] = to_bytes(pres_rels);
    const EditedDeck deck("minimal.pptx", changes);
    CHECK(run_rule(kRule, deck.pkg).empty());
}
