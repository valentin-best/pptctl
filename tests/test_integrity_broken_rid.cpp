// Tests for the "broken-rid" integrity rule (#130): a relationship-namespace
// attribute (r:embed, r:id, r:link, ...) naming an rId the part's .rels lacks.

#include <doctest/doctest.h>

#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <string>
#include <string_view>

#include "integrity_test_util.hpp"

using integrity_test::EditedDeck;
using integrity_test::run_rule;

namespace {

constexpr const char* kRule = "broken-rid";
constexpr const char* kSlide = "ppt/slides/slide1.xml";
constexpr std::string_view kTransitional =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

// A one-picture slide whose blip attribute is `attr="value"`, with `prefix`
// bound to `ns` on the root. minimal.pptx's slide 1 .rels defines only rId1.
std::string slide_xml(std::string_view prefix,
                      std::string_view ns,
                      std::string_view attr,
                      std::string_view value) {
    return std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)") +
           R"(<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )" +
           R"(xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:)" +
           std::string(prefix) + R"(=")" + std::string(ns) + R"(">)" +
           R"(<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>)" +
           R"(</p:nvGrpSpPr><p:grpSpPr/><p:pic><p:nvPicPr><p:cNvPr id="2" name="Pic"/>)" +
           R"(<p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip )" + std::string(prefix) + ":" +
           std::string(attr) + R"(=")" + std::string(value) +
           R"("/></p:blipFill><p:spPr/></p:pic></p:spTree></p:cSld></p:sld>)";
}

std::vector<pptctl::Diagnostic> lint_slide(const std::string& xml) {
    pptctl::PartChanges changes;
    changes.replaced[kSlide] = integrity_test::to_bytes(xml);
    const EditedDeck deck("minimal.pptx", changes);
    return run_rule(kRule, deck.pkg);
}

}  // namespace

TEST_CASE("broken-rid: the problems fixture has exactly its one injected defect") {
    const auto d = run_rule(kRule, integrity_test::fixture("integrity_problems.pptx"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rule_id == "broken-rid");
    CHECK(d[0].severity == pptctl::Severity::Error);
    CHECK(d[0].part == kSlide);
    CHECK(d[0].slide_index == 1);
    CHECK(d[0].message == R"(r:embed="rId99" is not defined in ppt/slides/_rels/slide1.xml.rels)");
}

TEST_CASE("broken-rid: every clean fixture is silent") {
    for (const auto& path : integrity_test::clean_fixtures()) {
        CAPTURE(path);
        CHECK(run_rule(kRule, path).empty());
    }
}

TEST_CASE("broken-rid: a defined rId is fine") {
    CHECK(lint_slide(slide_xml("r", kTransitional, "embed", "rId1")).empty());
}

TEST_CASE("broken-rid: the namespace is matched by URI, not by the r: prefix") {
    const auto d = lint_slide(slide_xml("rel", kTransitional, "link", "rId7"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.starts_with(R"(rel:link="rId7")"));
    CHECK(lint_slide(slide_xml("r", "urn:not-relationships", "embed", "rId7")).empty());
}

TEST_CASE("broken-rid: the Strict relationships namespace counts too") {
    CHECK(
        lint_slide(
            slide_xml("r", "http://purl.oclc.org/ooxml/officeDocument/relationships", "id", "rId7"))
            .size() == 1);
}

TEST_CASE("broken-rid: an empty value is not a reference") {
    CHECK(lint_slide(slide_xml("r", kTransitional, "embed", "")).empty());
}

TEST_CASE("broken-rid: a prefix bound on an inner element is honoured") {
    std::string xml = slide_xml("x", "urn:unused", "embed", "");
    const std::string blip = R"(<a:blip x:embed=""/>)";
    xml.replace(
        xml.find(blip),
        blip.size(),
        std::string(R"(<a:blip xmlns:q=")") + std::string(kTransitional) + R"(" q:embed="rId5"/>)");
    const auto d = lint_slide(xml);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.starts_with(R"(q:embed="rId5")"));
}

TEST_CASE("broken-rid: unreachable parts are left to orphan-part") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide42.xml"] =
        integrity_test::to_bytes(slide_xml("r", kTransitional, "embed", "rId9"));
    const EditedDeck deck("minimal.pptx", changes);
    CHECK(run_rule(kRule, deck.pkg).empty());
}
