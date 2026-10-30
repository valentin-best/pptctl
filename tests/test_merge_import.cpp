// Tests for importing a slide's part closure into another package (#135):
// MergeTarget name allocation and content types, PartImporter.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <pptctl/integrity.hpp>
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

using pptctl::detail::MergeTarget;
using pptctl::detail::PartImporter;

constexpr std::string_view kSlideRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_import_" + run + "_" + std::to_string(counter++));
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

// Stand-in for the slide-list rebuild (#136): append `slides` to the base's
// p:sldIdLst so the integrity rules, which check reachable parts, see them.
void list_slides(const pptctl::Package& base,
                 pptctl::PartChanges& changes,
                 const std::vector<std::string>& slides) {
    auto pres = to_string(base.read_part("ppt/presentation.xml"));
    auto rels = to_string(base.read_part("ppt/_rels/presentation.xml.rels"));
    std::string ids;
    std::string entries;
    for (std::size_t i = 0; i < slides.size(); ++i) {
        const auto rid = std::format("rIdMerged{}", i + 1);
        ids += std::format(R"(<p:sldId id="{}" r:id="{}"/>)", 1000 + i, rid);
        entries += std::format(R"(<Relationship Id="{}" Type="{}" Target="{}"/>)",
                               rid,
                               kSlideRel,
                               pptctl::relative_target("ppt/presentation.xml", slides[i]));
    }
    REQUIRE(pres.find("</p:sldIdLst>") != std::string::npos);
    pres.insert(pres.find("</p:sldIdLst>"), ids);
    rels.insert(rels.find("</Relationships>"), entries);
    changes.replaced["ppt/presentation.xml"] = to_bytes(pres);
    changes.replaced["ppt/_rels/presentation.xml.rels"] = to_bytes(rels);
}

// Write `base` with the target's changes plus the imported slides listed, and
// return every integrity diagnostic on the result.
std::vector<pptctl::Diagnostic> integrity_of(pptctl::Package& base,
                                             const MergeTarget& target,
                                             const std::vector<std::string>& slides,
                                             const fs::path& out) {
    auto changes = target.changes();
    list_slides(base, changes, slides);
    REQUIRE(base.write(out.string(), changes));
    pptctl::Package merged;
    REQUIRE(merged.open(out.string()));
    return pptctl::check_integrity(merged, pptctl::PartGraph::build(merged));
}

std::string messages(const std::vector<pptctl::Diagnostic>& ds) {
    std::string out;
    for (const auto& d : ds) {
        out += d.rule_id + ": " + d.message + "\n";
    }
    return out;
}

}  // namespace

// ── MergeTarget::allocate ─────────────────────────────────────────────────────

TEST_CASE("MergeTarget::allocate: next free index in the part's family") {
    pptctl::Package base;
    REQUIRE(base.open(fixture("merge_a.pptx")));  // slides 1-3, image1.png, 11 layouts
    MergeTarget target(base);
    CHECK(target.allocate("ppt/slides/slide1.xml") == "ppt/slides/slide4.xml");
    CHECK(target.allocate("ppt/slides/slide1.xml") == "ppt/slides/slide5.xml");
    CHECK(target.allocate("ppt/slideLayouts/slideLayout3.xml") ==
          "ppt/slideLayouts/slideLayout12.xml");
    CHECK(target.allocate("ppt/media/image7.png") == "ppt/media/image2.png");
    // A different extension is a different family.
    CHECK(target.allocate("ppt/media/image7.jpeg") == "ppt/media/image1.jpeg");
    // Families the base doesn't have start at 1.
    CHECK(target.allocate("ppt/charts/chart4.xml") == "ppt/charts/chart1.xml");
    CHECK(target.allocate("ppt/embeddings/Microsoft_Excel_Sheet1.xlsx") ==
          "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx");
    // Unnumbered names get a number; the unnumbered one counts as index 0.
    CHECK(target.allocate("ppt/media/logo.png") == "ppt/media/logo1.png");
    CHECK(target.allocate("ppt/media/logo.png") == "ppt/media/logo2.png");
    CHECK(target.allocate("ppt/media/noext") == "ppt/media/noext1");
    // Names compare case-insensitively; the source's spelling is kept.
    CHECK(target.allocate("PPT/Slides/Slide9.XML") == "PPT/Slides/Slide6.XML");
}

// ── MergeTarget content types ────────────────────────────────────────────────

TEST_CASE("MergeTarget::changes: Default for a new extension, Override when the Default differs") {
    pptctl::Package base;
    REQUIRE(base.open(fixture("merge_b.pptx")));  // no media, so no png Default
    REQUIRE(base.content_type("x.png").empty());
    MergeTarget target(base);
    CHECK_FALSE(target.changes().replaced.contains("[Content_Types].xml"));

    target.add_part("ppt/media/image1.png", to_bytes("png"), "image/png");
    target.add_part("ppt/media/image2.png", to_bytes("png"), "image/png");
    target.add_part("ppt/slides/slide3.xml",
                    to_bytes("<p:sld/>"),
                    "application/vnd.openxmlformats-officedocument.presentationml.slide+xml");
    target.add_part("ppt/slides/_rels/slide3.xml.rels",
                    to_bytes("<Relationships/>"),
                    "application/vnd.openxmlformats-package.relationships+xml");
    target.add_part("ppt/media/raw", to_bytes("raw"), "application/x-raw");
    target.add_part("ppt/media/untyped.bin", to_bytes("?"), "");

    const auto changes = target.changes();
    CHECK(changes.replaced.size() == 7);  // six parts + [Content_Types].xml
    const auto ct = to_string(changes.replaced.at("[Content_Types].xml"));
    CHECK(ct.find(R"(<Default Extension="png" ContentType="image/png"/>)") != std::string::npos);
    CHECK(ct.find(R"(PartName="/ppt/media/image1.png")") == std::string::npos);
    CHECK(
        ct.find(
            R"(<Override PartName="/ppt/slides/slide3.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>)") !=
        std::string::npos);
    CHECK(ct.find("slide3.xml.rels") == std::string::npos);  // covered by the rels Default
    CHECK(ct.find(R"(<Override PartName="/ppt/media/raw" ContentType="application/x-raw"/>)") !=
          std::string::npos);
    CHECK(ct.find("untyped.bin") == std::string::npos);
}

// ── PartImporter ──────────────────────────────────────────────────────────────

TEST_CASE("PartImporter: a chart slide with notes arrives whole and well-formed") {
    ScratchDir dir;
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_a.pptx")));
    REQUIRE(source.open(fixture("merge_b.pptx")));
    MergeTarget target(base);
    PartImporter importer(source, target);

    const auto slide = importer.import("ppt/slides/slide2.xml");
    CHECK(slide == "ppt/slides/slide4.xml");
    CHECK(importer.warnings().empty());
    const auto& map = importer.mapping();
    CHECK(map.at("ppt/charts/chart1.xml") == "ppt/charts/chart1.xml");
    CHECK(map.at("ppt/embeddings/Microsoft_Excel_Sheet1.xlsx") ==
          "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx");
    CHECK(map.at("ppt/notesSlides/notesSlide2.xml") == "ppt/notesSlides/notesSlide1.xml");
    CHECK(map.at("ppt/slideMasters/slideMaster1.xml") == "ppt/slideMasters/slideMaster2.xml");
    CHECK(map.at("ppt/theme/theme1.xml") == "ppt/theme/theme2.xml");
    // The master brings all eleven of its layouts.
    CHECK(std::count_if(map.begin(), map.end(), [](const auto& e) {
              return e.first.starts_with("ppt/slideLayouts/");
          }) == 11);
    // Slide 1 of the source isn't reachable from slide 2: not copied.
    CHECK_FALSE(map.contains("ppt/slides/slide1.xml"));

    // Bodies are byte-identical and every relationship keeps its id and
    // resolves to the copy of its original target.
    const auto changes = target.changes();
    for (const auto& from : importer.copied()) {
        const auto& to = map.at(from);
        CHECK(changes.replaced.at(to) == source.read_part(from));
    }
    const auto out = dir.path / "out.pptx";
    REQUIRE(base.write(out.string(), changes));
    pptctl::Package merged;
    REQUIRE(merged.open(out.string()));
    for (const auto& from : importer.copied()) {
        const auto before = source.relationships(from);
        const auto after = merged.relationships(map.at(from));
        REQUIRE(before.size() == after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            CHECK(after[i].id == before[i].id);
            CHECK(after[i].type == before[i].type);
            CHECK(after[i].target == map.at(before[i].target));
            CHECK(merged.part_size(after[i].target));
        }
        CHECK(merged.content_type(map.at(from)) == source.content_type(from));
    }
}

TEST_CASE("PartImporter: imported slides pass every integrity rule once listed") {
    ScratchDir dir;
    for (const auto* name : {"merge_b.pptx",
                             "merge_c.pptx",
                             "notes_and_masters.pptx",
                             "charts_deck.pptx",
                             "images_deck.pptx"}) {
        CAPTURE(name);
        pptctl::Package base;
        pptctl::Package source;
        REQUIRE(base.open(fixture("merge_a.pptx")));
        REQUIRE(source.open(fixture(name)));
        MergeTarget target(base);
        PartImporter importer(source, target);
        std::vector<std::string> slides;
        for (const auto& rel : source.relationships("ppt/presentation.xml")) {
            if (rel.type == kSlideRel) {
                slides.push_back(importer.import(rel.target));
            }
        }
        CHECK(importer.warnings().empty());
        const auto ds = integrity_of(base, target, slides, dir.path / name);
        CHECK_MESSAGE(ds.empty(), messages(ds));
    }
}

TEST_CASE("PartImporter: shared parts are copied once; links between slides follow them") {
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_a.pptx")));
    REQUIRE(source.open(fixture("merge_c.pptx")));
    MergeTarget target(base);
    PartImporter importer(source, target);
    // Slide 2 first: its link pulls slide 1 in, which then isn't copied twice.
    CHECK(importer.import("ppt/slides/slide2.xml") == "ppt/slides/slide4.xml");
    CHECK(importer.import("ppt/slides/slide1.xml") == "ppt/slides/slide5.xml");
    const auto copied = importer.copied();
    CHECK(std::count(copied.begin(), copied.end(), "ppt/slides/slide1.xml") == 1);
    CHECK(std::count(copied.begin(), copied.end(), "ppt/media/image1.png") == 1);
    CHECK(std::count(copied.begin(), copied.end(), "ppt/slideMasters/slideMaster1.xml") == 1);

    const auto changes = target.changes();
    const auto slide4 = to_string(changes.replaced.at("ppt/slides/_rels/slide4.xml.rels"));
    CHECK(
        slide4.find(
            R"(Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slide5.xml")") !=
        std::string::npos);
    // The external hyperlink survives untouched.
    const auto slide5 = to_string(changes.replaced.at("ppt/slides/_rels/slide5.xml.rels"));
    CHECK(slide5.find(R"(Target="https://example.com/pptctl" TargetMode="External")") !=
          std::string::npos);
    CHECK(slide5.find(R"(Target="../media/image2.png")") != std::string::npos);
}

TEST_CASE("PartImporter: a resolved part is referenced, not copied") {
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_a.pptx")));
    REQUIRE(source.open(fixture("merge_c.pptx")));
    MergeTarget target(base);
    std::vector<std::string> asked;
    std::vector<std::string> types;
    PartImporter importer(
        source,
        target,
        [&](std::string_view part, std::string_view rel_type) -> std::optional<std::string> {
            asked.emplace_back(part);
            types.emplace_back(rel_type);
            if (part.starts_with("ppt/slideLayouts/")) {
                return "ppt/slideLayouts/slideLayout2.xml";
            }
            return std::nullopt;
        });
    CHECK(importer.import("ppt/slides/slide1.xml") == "ppt/slides/slide4.xml");
    CHECK(asked.front() == "ppt/slides/slide1.xml");
    CHECK(types.front().empty());  // the part import() was called with
    CHECK(types[1] ==
          "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout");
    // The layout stands in for the source's, so neither it nor its master is copied.
    for (const auto& part : importer.copied()) {
        CHECK_FALSE(part.starts_with("ppt/slideLayouts/"));
        CHECK_FALSE(part.starts_with("ppt/slideMasters/"));
    }
    CHECK(importer.copied().size() == 2);  // the slide and the logo
    const auto rels = to_string(target.changes().replaced.at("ppt/slides/_rels/slide4.xml.rels"));
    CHECK(rels.find(R"(Target="../slideLayouts/slideLayout2.xml")") != std::string::npos);
}

TEST_CASE("PartImporter: a missing target or content type is kept as is and reported") {
    pptctl::Package base;
    pptctl::Package source;
    REQUIRE(base.open(fixture("merge_a.pptx")));
    REQUIRE(source.open(fixture("integrity_problems.pptx")));
    MergeTarget target(base);
    PartImporter importer(source, target);
    const auto slide = importer.import("ppt/slides/slide1.xml");
    REQUIRE_FALSE(slide.empty());
    // The fixture's slide 1 also shows a picture with no content type.
    CHECK(importer.warnings() ==
          std::vector<std::string>{
              "ppt/slides/slide1.xml: relationship rId4 targets ppt/media/missing.png, which is "
              "missing",
              "ppt/media/extra.webp: no content type declared"});
    const auto rels = to_string(target.changes().replaced.at(pptctl::rels_part_for(slide)));
    CHECK(rels.find(R"(Id="rId4")") != std::string::npos);

    CHECK(importer.import("ppt/slides/nope.xml").empty());
}
