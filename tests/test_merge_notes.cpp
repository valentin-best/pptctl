// Tests for merge's notes-slide repair (#166): notes slides without
// p:clrMapOvr, which Keynote refuses, get the one PowerPoint writes.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/merge_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kClrMapOvr = "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>";
constexpr std::string_view kNotesSlideType =
    "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";

std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name);
}

std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string to_string(const std::vector<std::byte>& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
    return out;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_notes_" + run + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
    static inline int counter = 0;
};

// `fixture_name` with p:clrMapOvr taken out of every notes slide, as
// LibreOffice and ONLYOFFICE write them, saved in `dir`.
std::string without_notes_clrmap(const ScratchDir& dir, std::string_view fixture_name) {
    pptctl::Package src;
    REQUIRE(src.open(fixture(fixture_name)));
    pptctl::PartChanges changes;
    for (const auto& part : src.part_names()) {
        if (src.content_type(part) != kNotesSlideType) {
            continue;
        }
        auto xml = to_string(src.read_part(part));
        const auto at = xml.find(kClrMapOvr);
        REQUIRE(at != std::string::npos);
        xml.erase(at, kClrMapOvr.size());
        changes.replaced[part] = to_bytes(xml);
    }
    REQUIRE(!changes.replaced.empty());
    const auto out = (dir.path / ("stripped_" + std::string(fixture_name))).string();
    REQUIRE(src.write(out, changes));
    return out;
}

std::size_t notes_clrmap_findings(const std::string& path) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(path));
    const auto graph = pptctl::PartGraph::build(pkg);
    const auto d = pptctl::check_integrity(pkg, graph);
    return static_cast<std::size_t>(std::count_if(
        d.begin(), d.end(), [](const auto& x) { return x.rule_id == "notes-clrmap"; }));
}

pptctl::MergeReport merge_ok(const std::vector<std::string>& inputs, const std::string& out) {
    std::string error;
    const auto report = pptctl::merge(inputs, out, {}, error);
    REQUIRE_MESSAGE(report, error);
    return *report;
}

bool mentions_clrmap(const pptctl::MergeInputReport& input) {
    return std::any_of(input.warnings.begin(), input.warnings.end(), [](const auto& w) {
        return w.find("p:clrMapOvr") != std::string::npos;
    });
}

// `root`, after an XML declaration, set as the notes slide of a
// merge_b-based target and repaired; the result without its declaration,
// which must come through unchanged.
std::string repaired(std::string_view root) {
    constexpr std::string_view kDecl = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
    pptctl::Package base;
    REQUIRE(base.open(fixture("merge_b.pptx")));
    pptctl::detail::MergeTarget target(base);
    const std::string part = "ppt/notesSlides/notesSlide1.xml";
    target.replace_part(part, to_bytes(std::string(kDecl) + std::string(root)));
    const std::vector<std::string> notes{part};
    CHECK(pptctl::detail::add_notes_clr_map_ovr(target, notes) == 1);
    const auto out = to_string(target.read_part(part));
    REQUIRE(out.starts_with(kDecl));
    return out.substr(kDecl.size());
}

}  // namespace

TEST_CASE("merge notes: imported notes slides without p:clrMapOvr get it, and a warning") {
    const ScratchDir dir;
    const auto b = without_notes_clrmap(dir, "merge_b.pptx");
    REQUIRE(notes_clrmap_findings(b) == 2);
    const auto out = (dir.path / "out.pptx").string();
    const auto report = merge_ok({fixture("merge_a.pptx"), b}, out);
    CHECK(notes_clrmap_findings(out) == 0);
    CHECK_FALSE(mentions_clrmap(report.inputs[0]));
    // Its other warning is about b's chart (#167).
    REQUIRE(report.inputs[1].warnings.size() == 2);
    CHECK(report.inputs[1].warnings[1] ==
          "added p:clrMapOvr to 2 notes slides that lacked it "
          "(Keynote refuses to open a deck without it)");
}

TEST_CASE("merge notes: the base deck's notes slides are repaired too") {
    const ScratchDir dir;
    const auto b = without_notes_clrmap(dir, "merge_b.pptx");
    const auto out = (dir.path / "out.pptx").string();
    const auto report = merge_ok({b, fixture("merge_a.pptx")}, out);
    CHECK(notes_clrmap_findings(out) == 0);
    CHECK(mentions_clrmap(report.inputs[0]));
    CHECK_FALSE(mentions_clrmap(report.inputs[1]));
}

TEST_CASE("merge notes: notes slides that have it are copied byte for byte, silently") {
    const ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    const auto report = merge_ok({fixture("merge_a.pptx"), fixture("merge_b.pptx")}, out);
    for (const auto& input : report.inputs) {
        CHECK_FALSE(mentions_clrmap(input));
    }
    pptctl::Package src;
    pptctl::Package merged;
    REQUIRE(src.open(fixture("merge_b.pptx")));
    REQUIRE(merged.open(out));
    std::vector<std::vector<std::byte>> want;
    std::vector<std::vector<std::byte>> got;
    for (const auto& part : src.part_names()) {
        if (src.content_type(part) == kNotesSlideType) {
            want.push_back(src.read_part(part));
        }
    }
    for (const auto& part : merged.part_names()) {
        if (merged.content_type(part) == kNotesSlideType) {
            got.push_back(merged.read_part(part));
        }
    }
    REQUIRE(want.size() == 2);
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    CHECK(got == want);
}

TEST_CASE("add_notes_clr_map_ovr: placement and prefixes") {
    SUBCASE("appended after p:cSld, with the root's DrawingML prefix") {
        CHECK(
            repaired(R"(<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
                     R"(xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
                     R"(<p:cSld/></p:notes>)") ==
            R"(<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
            R"(xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
            R"(<p:cSld/><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>)");
    }
    SUBCASE("before a trailing p:extLst, declaring DrawingML when the root doesn't") {
        CHECK(
            repaired(
                R"(<p:notes xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
                R"(<p:cSld/><p:extLst/></p:notes>)") ==
            R"(<p:notes xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
            R"(<p:cSld/><p:clrMapOvr><a:masterClrMapping )"
            R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"/>)"
            R"(</p:clrMapOvr><p:extLst/></p:notes>)");
    }
    SUBCASE("an unprefixed root gets unprefixed elements") {
        CHECK(
            repaired(R"(<notes xmlns="http://schemas.openxmlformats.org/presentationml/2006/main" )"
                     R"(xmlns:d="http://schemas.openxmlformats.org/drawingml/2006/main">)"
                     R"(<cSld/></notes>)") ==
            R"(<notes xmlns="http://schemas.openxmlformats.org/presentationml/2006/main" )"
            R"(xmlns:d="http://schemas.openxmlformats.org/drawingml/2006/main">)"
            R"(<cSld/><clrMapOvr><d:masterClrMapping/></clrMapOvr></notes>)");
    }
}

TEST_CASE("add_notes_clr_map_ovr: parts that have it or don't parse are left alone") {
    pptctl::Package base;
    REQUIRE(base.open(fixture("merge_b.pptx")));
    pptctl::detail::MergeTarget target(base);
    const std::string broken = "ppt/notesSlides/notesSlide2.xml";
    target.replace_part(broken, to_bytes("<p:notes><p:cSld>"));
    const std::vector<std::string> notes{"ppt/notesSlides/notesSlide1.xml", broken};
    CHECK(pptctl::detail::add_notes_clr_map_ovr(target, notes) == 0);
    CHECK(target.read_part(notes[0]) == base.read_part(notes[0]));
    CHECK(to_string(target.read_part(broken)) == "<p:notes><p:cSld>");
}
