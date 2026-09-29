// Tests for the diff engine: slide fingerprints, similarity, matching.

#include <doctest/doctest.h>

#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/slide.hpp>
#include <sstream>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace pptctl::cmd {
int diff_to(const std::string& a, const std::string& b, std::ostream& os, const DiffOptions& opts);
}  // namespace pptctl::cmd

namespace {

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A slide with one text shape whose paragraph is `text`. Shape id/name are
// fixed so two slides built from the same text are structurally identical.
std::string text_slide(const std::string& text, const char* name = "Title 1") {
    return std::string(R"(<?xml version="1.0"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
       xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld><p:spTree>
    <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
    <p:grpSpPr/>
    <p:sp>
      <p:nvSpPr><p:cNvPr id="2" name=")") +
           name + R"("/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
      <p:spPr><a:xfrm><a:off x="100" y="200"/><a:ext cx="300" cy="400"/></a:xfrm></p:spPr>
      <p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>)" +
           text + R"(</a:t></a:r></a:p></p:txBody>
    </p:sp>
  </p:spTree></p:cSld>
</p:sld>)";
}

// Same as text_slide but with the shape nudged to a different offset.
std::string nudged_slide(const std::string& text) {
    std::string xml = text_slide(text);
    const auto pos = xml.find(R"(x="100")");
    xml.replace(pos, 7, R"(x="999")");
    return xml;
}

pptctl::SlideSummary summary_of(const std::string& xml, std::size_t index) {
    pptctl::Slide s;
    REQUIRE(s.parse(xml));
    s.set_index(index);
    return pptctl::summarize(s);
}

std::vector<pptctl::SlideSummary> deck(const std::vector<std::string>& xmls) {
    std::vector<pptctl::SlideSummary> out;
    for (std::size_t i = 0; i < xmls.size(); ++i) {
        out.push_back(summary_of(xmls[i], i + 1));
    }
    return out;
}

}  // namespace

TEST_CASE("Diff: fingerprint — identical slides hash equal, edited text differs") {
    const auto a = summary_of(text_slide("Quarterly results"), 1);
    const auto b = summary_of(text_slide("Quarterly results"), 7);
    const auto c = summary_of(text_slide("Quarterly result"), 1);
    CHECK(a.fingerprint == b.fingerprint);  // index is not part of identity
    CHECK(a.fingerprint != c.fingerprint);
}

TEST_CASE("Diff: fingerprint — moving a shape changes the fingerprint") {
    const auto a = summary_of(text_slide("Agenda"), 1);
    const auto b = summary_of(nudged_slide("Agenda"), 1);
    CHECK(a.fingerprint != b.fingerprint);
}

TEST_CASE("Diff: similarity — same text is 1, unrelated text is 0, partial in between") {
    const auto a = summary_of(text_slide("alpha beta gamma delta"), 1);
    const auto same = summary_of(nudged_slide("alpha beta gamma delta"), 1);
    const auto partial = summary_of(text_slide("alpha beta epsilon zeta"), 1);
    const auto other = summary_of(text_slide("one two three four"), 1);
    CHECK(pptctl::slide_similarity(a, same) == doctest::Approx(1.0));
    CHECK(pptctl::slide_similarity(a, other) == doctest::Approx(0.0));
    const double p = pptctl::slide_similarity(a, partial);
    CHECK(p > 0.3);
    CHECK(p < 0.7);
}

TEST_CASE("Diff: match — identical decks yield all-unchanged, no moves") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    CHECK(report.identical());
    REQUIRE(report.slides.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(report.slides[i].status == pptctl::SlideStatus::Unchanged);
        CHECK(report.slides[i].a_index == i + 1);
        CHECK(report.slides[i].b_index == i + 1);
        CHECK_FALSE(report.slides[i].moved);
    }
}

TEST_CASE("Diff: match — reordered slides are matched by fingerprint and flagged moved") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("two"), text_slide("three"), text_slide("one")});
    const auto report = pptctl::match_slides(a, b, {});
    CHECK_FALSE(report.identical());
    REQUIRE(report.slides.size() == 3);
    std::size_t moved = 0;
    for (const auto& s : report.slides) {
        CHECK(s.status == pptctl::SlideStatus::Unchanged);
        if (s.moved) {
            ++moved;
            CHECK(s.a_index == 1);
            CHECK(s.b_index == 3);
        }
    }
    CHECK(moved == 1);  // "one" moved to the end; "two"/"three" kept their order
}

TEST_CASE("Diff: match — added slide is reported at its B position") {
    const auto a = deck({text_slide("one"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 3);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Added);
    CHECK(report.slides[1].a_index == 0);
    CHECK(report.slides[1].b_index == 2);
    // Inserting shifts B indices; that is not a move.
    CHECK(report.slides[2].status == pptctl::SlideStatus::Unchanged);
    CHECK_FALSE(report.slides[2].moved);
}

TEST_CASE("Diff: match — removed slide is reported at its A position") {
    const auto a = deck({text_slide("one"), text_slide("two"), text_slide("three")});
    const auto b = deck({text_slide("one"), text_slide("three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 3);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Removed);
    CHECK(report.slides[1].a_index == 2);
    CHECK(report.slides[1].b_index == 0);
}

TEST_CASE("Diff: match — near-duplicate pairs as changed via best-match") {
    const auto a = deck({text_slide("one"), text_slide("revenue grew ten percent this quarter")});
    const auto b =
        deck({text_slide("one"), text_slide("revenue grew twelve percent this quarter")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Changed);
    CHECK(report.slides[1].a_index == 2);
    CHECK(report.slides[1].b_index == 2);
    CHECK(report.slides[1].similarity > 0.5);
}

TEST_CASE("Diff: match — below threshold is added + removed, not changed") {
    const auto a = deck({text_slide("alpha beta gamma")});
    const auto b = deck({text_slide("one two three")});
    const auto report = pptctl::match_slides(a, b, {});
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[0].status == pptctl::SlideStatus::Removed);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Added);
}

TEST_CASE("Diff: match — best-match prefers the most similar candidate") {
    // A2 has two plausible partners in B; the closer one must win.
    const auto a = deck({text_slide("alpha beta gamma delta")});
    const auto b = deck({text_slide("alpha beta zero zero"), text_slide("alpha beta gamma zero")});
    const auto report = pptctl::match_slides(a, b, {});
    bool paired_with_2 = false;
    for (const auto& s : report.slides) {
        if (s.status == pptctl::SlideStatus::Changed) {
            paired_with_2 = (s.b_index == 2);
        }
    }
    CHECK(paired_with_2);
}

TEST_CASE("Diff: match — empty decks are identical") {
    const auto report = pptctl::match_slides({}, {}, {});
    CHECK(report.identical());
    CHECK(report.slides.empty());
}

TEST_CASE("Diff: command — self-diff of a fixture is identical, exit 0") {
    std::ostringstream os;
    const int rc =
        pptctl::cmd::diff_to(fixture("three_slides.pptx"), fixture("three_slides.pptx"), os, {});
    CHECK(rc == 0);
    CHECK(os.str().empty());
}

TEST_CASE("Diff: command — different fixtures exit 1 and describe slide ops") {
    std::ostringstream os;
    const int rc =
        pptctl::cmd::diff_to(fixture("minimal.pptx"), fixture("three_slides.pptx"), os, {});
    CHECK(rc == 1);
    CHECK(os.str().find("added") != std::string::npos);
}

TEST_CASE("Diff: command — unreadable input exits 2") {
    std::ostringstream os;
    const int rc = pptctl::cmd::diff_to(fixture("nope.pptx"), fixture("minimal.pptx"), os, {});
    CHECK(rc == 2);
}

// ── op seams (scaffold for #70 / #72 / #73) ───────────────────────────────────

namespace pptctl::cmd {
std::optional<DiffReport> diff_report(const std::string& a,
                                      const std::string& b,
                                      const DiffOptions& opts);
}  // namespace pptctl::cmd

TEST_CASE("Diff: compare_slides — identical slides yield no changes") {
    pptctl::Slide a;
    pptctl::Slide b;
    REQUIRE(a.parse(text_slide("same")));
    REQUIRE(b.parse(text_slide("same")));
    const auto changes = pptctl::compare_slides(a, b, {});
    CHECK(changes.empty());
}

TEST_CASE("Diff: diff_report — changed pairs carry a (possibly empty) change set") {
    const auto r =
        pptctl::cmd::diff_report(fixture("three_slides.pptx"), fixture("three_slides.pptx"), {});
    REQUIRE(r);
    CHECK(r->identical());
    for (const auto& s : r->slides) {
        CHECK(s.changes.empty());
    }
}

TEST_CASE("Diff: diff_report — unreadable input yields nullopt") {
    CHECK_FALSE(pptctl::cmd::diff_report(fixture("nope.pptx"), fixture("minimal.pptx"), {}));
}
