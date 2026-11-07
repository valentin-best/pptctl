// Tests for the "notes-clrmap" integrity rule (#166): a speaker-notes slide
// without p:clrMapOvr, which Keynote refuses.

#include <doctest/doctest.h>

#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>
#include <string_view>

#include "../src/lib/integrity_internal.hpp"
#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::run_rule;
using integrity_test::to_bytes;
using integrity_test::to_string;

namespace {

constexpr const char* kRule = "notes-clrmap";
constexpr std::string_view kClrMapOvr = "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>";

// `fixture_name`'s `part` with `from` replaced by `to` (once).
pptctl::PartChanges edit_part(std::string_view fixture_name,
                              const std::string& part,
                              std::string_view from,
                              std::string_view to) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(integrity_test::fixture(fixture_name)));
    auto xml = to_string(pkg.read_part(part));
    const auto at = xml.find(from);
    REQUIRE(at != std::string::npos);
    xml.replace(at, from.size(), to);
    pptctl::PartChanges changes;
    changes.replaced[part] = to_bytes(xml);
    return changes;
}

}  // namespace

TEST_CASE("notes-clrmap: the problems fixture has exactly its one injected defect") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "notes-clrmap");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == "ppt/notesSlides/notesSlide1.xml");
    CHECK(d[0].slide_index == 0);
    CHECK(d[0].message ==
          "notes slide has no p:clrMapOvr: Keynote refuses to open the deck "
          "(PowerPoint writes <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>)");
}

TEST_CASE("notes-clrmap: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("notes-clrmap: each notes slide that lacks it is reported, and only those") {
    // notes_and_masters has four notes slides; strip the element from two.
    auto changes =
        edit_part("notes_and_masters.pptx", "ppt/notesSlides/notesSlide2.xml", kClrMapOvr, "");
    changes.replaced.merge(
        edit_part("notes_and_masters.pptx", "ppt/notesSlides/notesSlide4.xml", kClrMapOvr, "")
            .replaced);
    const EditedDeck deck("notes_and_masters.pptx", changes);
    const auto d = run_rule(kRule, deck.pkg);
    REQUIRE(d.size() == 2);
    CHECK(d[0].part == "ppt/notesSlides/notesSlide2.xml");
    CHECK(d[1].part == "ppt/notesSlides/notesSlide4.xml");
}

TEST_CASE("notes-clrmap: slides without p:clrMapOvr are fine (Keynote opens them)") {
    const auto changes =
        edit_part("notes_and_masters.pptx", "ppt/slides/slide1.xml", kClrMapOvr, "");
    const EditedDeck deck("notes_and_masters.pptx", changes);
    CHECK(run_rule(kRule, deck.pkg).empty());
}

TEST_CASE("notes_lack_clr_map_ovr: matches by local name, at the root only") {
    using pptctl::detail::notes_lack_clr_map_ovr;
    constexpr std::string_view kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
    const auto doc = [&](std::string_view body) {
        return to_bytes(std::string(R"(<notes xmlns=")") + std::string(kP) + R"(">)" +
                        std::string(body) + "</notes>");
    };
    CHECK(notes_lack_clr_map_ovr(doc("<cSld/>")));
    CHECK_FALSE(notes_lack_clr_map_ovr(doc("<cSld/><clrMapOvr/>")));
    CHECK(notes_lack_clr_map_ovr(doc("<cSld><clrMapOvr/></cSld>")));
    CHECK_FALSE(notes_lack_clr_map_ovr(to_bytes("<p:sld><p:cSld/></p:sld>")));
    CHECK_FALSE(notes_lack_clr_map_ovr(to_bytes("<p:notes><p:cSld>")));  // doesn't parse
}
