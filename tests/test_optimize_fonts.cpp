// Tests for the "fonts" optimize pass (#101).
//
// No fixture embeds fonts (python-pptx can't), so the tests inject them into a
// copy of three_slides.pptx the way PowerPoint stores them: font parts under
// ppt/fonts/, font relationships from the presentation, a p:embeddedFontLst in
// ppt/presentation.xml and the fntdata Default in [Content_Types].xml.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/presentation.hpp>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/optimize_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_optimize_fonts_" + run + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    static inline int counter = 0;
};

std::string to_string(const std::vector<std::byte>& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
    return out;
}

std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Insert `text` before the first occurrence of `anchor` in `s`.
std::string insert_before(std::string s, std::string_view anchor, std::string_view text) {
    const auto at = s.find(anchor);
    REQUIRE(at != std::string::npos);
    s.insert(at, text);
    return s;
}

constexpr std::string_view kFontRelType =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/font";

// The fonts injected by fonts_deck: (typeface, style element, part, rId).
struct InjectedFont {
    std::string_view typeface;
    std::string_view style;  // p:regular, p:bold, p:italic or p:boldItalic
    std::string_view part;
    std::string_view rid;
};
constexpr InjectedFont kFonts[] = {
    {"Lato", "p:regular", "ppt/fonts/font1.fntdata", "rId10"},
    {"Lato", "p:bold", "ppt/fonts/font2.fntdata", "rId11"},
    {"Source Serif", "p:regular", "ppt/fonts/font3.fntdata", "rId12"},
    {"Source Serif", "p:italic", "ppt/fonts/font4.fntdata", "rId13"},
    {"Source Serif", "p:boldItalic", "ppt/fonts/font5.fntdata", "rId14"},
};

// The p:embeddedFontLst for kFonts, one p:embeddedFont per typeface.
std::string embedded_font_list() {
    std::string xml = "<p:embeddedFontLst>";
    std::string_view open;
    for (const auto& f : kFonts) {
        if (f.typeface != open) {
            if (!open.empty()) {
                xml += "</p:embeddedFont>";
            }
            xml += R"(<p:embeddedFont><p:font typeface=")" + std::string(f.typeface) +
                   R"(" pitchFamily="2" charset="0"/>)";
            open = f.typeface;
        }
        xml += "<" + std::string(f.style) + R"( r:id=")" + std::string(f.rid) + R"("/>)";
    }
    return xml + "</p:embeddedFont></p:embeddedFontLst>";
}

// Copy three_slides.pptx to `out` with kFonts embedded. Every font part holds
// `font_bytes`; when that's empty, part i holds arbitrary bytes repeated i+1
// times, so each part has a distinct size. `font_list` is the p:embeddedFontLst.
void write_fonts_deck(const std::string& out,
                      const std::vector<std::byte>& font_bytes = {},
                      const std::string& font_list = embedded_font_list()) {
    pptctl::Package src;
    REQUIRE(src.open(fixture("three_slides.pptx")));
    pptctl::PartChanges changes;

    // CT_Presentation: ... sldSz, notesSz, smartTags, embeddedFontLst, custShowLst,
    // photoAlbum, custDataLst, kinsoku, defaultTextStyle, ... — three_slides has
    // nothing between notesSz and defaultTextStyle.
    auto pres = to_string(src.read_part("ppt/presentation.xml"));
    pres = insert_before(pres, "<p:defaultTextStyle>", font_list);
    pres = insert_before(pres, R"(saveSubsetFonts="1")", R"(embedTrueTypeFonts="1" )");
    changes.replaced["ppt/presentation.xml"] = to_bytes(pres);

    std::string rels_add;
    for (const auto& f : kFonts) {
        rels_add += R"(<Relationship Id=")" + std::string(f.rid) + R"(" Type=")" +
                    std::string(kFontRelType) + R"(" Target="fonts/)" +
                    std::string(f.part.substr(f.part.rfind('/') + 1)) + R"("/>)";
    }
    changes.replaced["ppt/_rels/presentation.xml.rels"] = to_bytes(insert_before(
        to_string(src.read_part("ppt/_rels/presentation.xml.rels")), "</Relationships>", rels_add));

    changes.replaced["[Content_Types].xml"] = to_bytes(
        insert_before(to_string(src.read_part("[Content_Types].xml")),
                      "</Types>",
                      R"(<Default Extension="fntdata" ContentType="application/x-fontdata"/>)"));

    const auto unit = to_bytes("not really a font, but bytes");
    std::size_t copies = 1;
    for (const auto& f : kFonts) {
        std::vector<std::byte> bytes = font_bytes;
        for (std::size_t i = 0; bytes.empty() && i < copies; ++i) {
            bytes.insert(bytes.end(), unit.begin(), unit.end());
        }
        changes.replaced[std::string(f.part)] = std::move(bytes);
        ++copies;
    }
    REQUIRE(src.write(out, changes));
}

// Only the fonts pass on, so other passes can't touch the deck.
pptctl::OptimizeOptions fonts_only() {
    pptctl::OptimizeOptions opts;
    opts.layouts = false;
    opts.crop = false;
    opts.images = false;
    opts.orphans = false;
    return opts;
}

std::set<std::string> font_parts() {
    std::set<std::string> out;
    for (const auto& f : kFonts) {
        out.emplace(f.part);
    }
    return out;
}

}  // namespace

// ── the pass ──────────────────────────────────────────────────────────────────

TEST_CASE("optimize fonts: removes every embedded font part and reports it") {
    ScratchDir dir;
    const auto in = (dir.path / "fonts.pptx").string();
    write_fonts_deck(in);
    pptctl::Package pkg;
    REQUIRE(pkg.open(in));
    const auto graph = pptctl::PartGraph::build(pkg);

    const auto result = pptctl::detail::optimize_fonts(pkg, graph, {});
    CHECK(result.changes.removed == font_parts());
    // The pass edits presentation.xml only; complete_removals does the rels.
    CHECK(result.changes.replaced.size() == 1);
    CHECK(result.changes.replaced.contains("ppt/presentation.xml"));

    REQUIRE(result.actions.size() == std::size(kFonts));
    const std::vector<std::string> details{
        "Lato", "Lato Bold", "Source Serif", "Source Serif Italic", "Source Serif Bold Italic"};
    for (std::size_t i = 0; i < result.actions.size(); ++i) {
        const auto& a = result.actions[i];
        CAPTURE(i);
        CHECK(a.part == kFonts[i].part);
        CHECK(a.kind == "removed");
        CHECK(a.detail == details[i]);
        CHECK(a.bytes_before == pkg.part_size(a.part).value_or(0));
        CHECK(a.bytes_before > 0);
        CHECK(a.bytes_after == 0);
    }
}

TEST_CASE("optimize fonts: drops p:embeddedFontLst and embedTrueTypeFonts, nothing else") {
    ScratchDir dir;
    const auto in = (dir.path / "fonts.pptx").string();
    write_fonts_deck(in);
    pptctl::Package pkg;
    REQUIRE(pkg.open(in));
    const auto result =
        pptctl::detail::optimize_fonts(pkg, pptctl::PartGraph::build(pkg), fonts_only());
    REQUIRE(result.changes.replaced.contains("ppt/presentation.xml"));
    const auto pres = to_string(result.changes.replaced.at("ppt/presentation.xml"));

    CHECK_FALSE(contains(pres, "embeddedFont"));
    CHECK_FALSE(contains(pres, "embedTrueTypeFonts"));
    // saveSubsetFonts is PowerPoint's default even without embedding; it's inert
    // once embedTrueTypeFonts is gone, so it stays.
    CHECK(contains(pres, R"(saveSubsetFonts="1")"));

    // Exactly the original presentation.xml, re-serialized raw: declaration kept
    // (pugixml writes it double-quoted), no indentation, every other node as-is.
    pptctl::Package original;
    REQUIRE(original.open(fixture("three_slides.pptx")));
    auto expected = to_string(original.read_part("ppt/presentation.xml"));
    const auto decl_end = expected.find("?>\n");
    REQUIRE(decl_end != std::string::npos);
    expected.replace(0, decl_end + 3, R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)");
    CHECK(pres == expected);
}

TEST_CASE("optimize fonts: no-op on decks without embedded fonts") {
    for (const char* name :
         {"minimal.pptx", "three_slides.pptx", "images_deck.pptx", "notes_and_masters.pptx"}) {
        CAPTURE(name);
        pptctl::Package pkg;
        REQUIRE(pkg.open(fixture(name)));
        const auto result = pptctl::detail::optimize_fonts(pkg, pptctl::PartGraph::build(pkg), {});
        CHECK(result.changes.replaced.empty());
        CHECK(result.changes.removed.empty());
        CHECK(result.actions.empty());
    }
}

TEST_CASE("optimize fonts: an empty p:embeddedFontLst is left alone") {
    ScratchDir dir;
    const auto in = (dir.path / "empty_list.pptx").string();
    write_fonts_deck(in, {}, "<p:embeddedFontLst/>");
    pptctl::Package pkg;
    REQUIRE(pkg.open(in));
    const auto result = pptctl::detail::optimize_fonts(pkg, pptctl::PartGraph::build(pkg), {});
    CHECK(result.changes.replaced.empty());
    CHECK(result.changes.removed.empty());
    CHECK(result.actions.empty());
}

TEST_CASE("optimize fonts: a list naming missing font parts is still removed") {
    ScratchDir dir;
    const auto in = (dir.path / "dangling.pptx").string();
    write_fonts_deck(in,
                     {},
                     R"(<p:embeddedFontLst><p:embeddedFont><p:font typeface="Ghost"/>)"
                     R"(<p:regular r:id="rId99"/></p:embeddedFont></p:embeddedFontLst>)");
    pptctl::Package pkg;
    REQUIRE(pkg.open(in));
    const auto result = pptctl::detail::optimize_fonts(pkg, pptctl::PartGraph::build(pkg), {});
    // rId99 resolves to nothing, so no part goes and nothing is reported, but
    // the list itself (and the embedding switch) is gone.
    CHECK(result.changes.removed.empty());
    CHECK(result.actions.empty());
    REQUIRE(result.changes.replaced.contains("ppt/presentation.xml"));
    const auto pres = to_string(result.changes.replaced.at("ppt/presentation.xml"));
    CHECK_FALSE(contains(pres, "embeddedFontLst"));
    CHECK_FALSE(contains(pres, "embedTrueTypeFonts"));
}

// ── end to end ────────────────────────────────────────────────────────────────

TEST_CASE("optimize fonts: end to end, fonts stripped and everything else untouched") {
    ScratchDir dir;
    const auto in = (dir.path / "fonts.pptx").string();
    const auto out = (dir.path / "out.pptx").string();
    write_fonts_deck(in);

    std::string error;
    const auto report = pptctl::optimize(in, out, fonts_only(), error);
    REQUIRE_MESSAGE(report, error);
    REQUIRE(report->passes.size() == 1);
    CHECK(report->passes[0].id == "fonts");
    CHECK(report->passes[0].actions.size() == std::size(kFonts));
    CHECK(report->output_bytes < report->input_bytes);

    pptctl::Package before;
    pptctl::Package after;
    REQUIRE(before.open(in));
    REQUIRE(after.open(out));

    // No font parts, and nothing left dangling or unreachable.
    for (const auto& name : after.part_names()) {
        CHECK_FALSE(name.starts_with("ppt/fonts/"));
    }
    const auto graph = pptctl::PartGraph::build(after);
    CHECK(graph.unreachable().empty());
    const auto rels = to_string(after.read_part("ppt/_rels/presentation.xml.rels"));
    CHECK_FALSE(contains(rels, kFontRelType));
    CHECK(contains(rels, "slides/slide3.xml"));

    pptctl::Presentation pres(after);
    REQUIRE(pres.load());
    CHECK(pres.slide_count() == 3);
    CHECK(pres.embedded_fonts().empty());

    // Every other part, [Content_Types].xml included (fntdata is a Default, which
    // an unused extension may keep), is byte-identical.
    const std::set<std::string> edited{"ppt/presentation.xml", "ppt/_rels/presentation.xml.rels"};
    const auto removed = font_parts();
    std::size_t compared = 0;
    for (const auto& name : before.part_names()) {
        if (edited.contains(name) || removed.contains(name)) {
            continue;
        }
        CAPTURE(name);
        CHECK(after.read_part(name) == before.read_part(name));
        ++compared;
    }
    CHECK(after.part_count() == compared + edited.size());
}

TEST_CASE("optimize fonts: opts.fonts = false leaves the deck untouched") {
    ScratchDir dir;
    const auto in = (dir.path / "fonts.pptx").string();
    const auto out = (dir.path / "out.pptx").string();
    write_fonts_deck(in);

    auto opts = fonts_only();
    opts.fonts = false;
    std::string error;
    const auto report = pptctl::optimize(in, out, opts, error);
    REQUIRE_MESSAGE(report, error);
    CHECK(report->passes.empty());

    pptctl::Package before;
    pptctl::Package after;
    REQUIRE(before.open(in));
    REQUIRE(after.open(out));
    CHECK(after.part_names() == before.part_names());
    for (const auto& name : before.part_names()) {
        CAPTURE(name);
        CHECK(after.read_part(name) == before.read_part(name));
    }
}

// Not part of the suite: writes build/manual_check/fonts_input.pptx and
// fonts_stripped.pptx (relative to the working directory) for renderer checks.
// Run from the repo root with:
//   ./build/macos-release/pptctl_tests --test-case="*manual check*" --no-skip
TEST_CASE("optimize fonts: manual check decks" * doctest::skip()) {
    const fs::path dir = "build/manual_check";
    fs::create_directories(dir);
    std::vector<std::byte> ttf;
    const fs::path font = "/System/Library/Fonts/Supplemental/NotoSansLycian-Regular.ttf";
    if (std::ifstream f(font, std::ios::binary); f) {
        const std::string raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        ttf = to_bytes(raw);
    }
    const auto in = (dir / "fonts_input.pptx").string();
    const auto out = (dir / "fonts_stripped.pptx").string();
    write_fonts_deck(in, ttf);
    std::string error;
    const auto report = pptctl::optimize(in, out, fonts_only(), error);
    REQUIRE_MESSAGE(report, error);
    MESSAGE("wrote " << in << " and " << out);
}
