// Tests for PartGraph: reachability and the reverse reference index.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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
               fs::path("pptctl_graph_" + run + "_" + std::to_string(counter++));
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

std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

constexpr std::string_view kRelsHead =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
    R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
constexpr std::string_view kRelsTail = "</Relationships>";
constexpr std::string_view kImageType =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view kLayoutType =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout";

std::string rel(std::string_view id, std::string_view type, std::string_view target) {
    return std::string(R"(<Relationship Id=")") + std::string(id) + R"(" Type=")" +
           std::string(type) + R"(" Target=")" + std::string(target) + R"("/>)";
}

// Write images_deck.pptx with `changes` applied and open the result.
pptctl::Package images_deck_with(const ScratchDir& dir, const pptctl::PartChanges& changes) {
    pptctl::Package src;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    const auto out = (dir.path / "out.pptx").string();
    REQUIRE(src.write(out, changes));
    pptctl::Package pkg;
    REQUIRE(pkg.open(out));
    return pkg;
}

}  // namespace

TEST_CASE("PartGraph: every part of a well-formed deck is reachable from the root") {
    for (const char* name : {"images_deck.pptx", "notes_and_masters.pptx", "three_slides.pptx"}) {
        CAPTURE(name);
        pptctl::Package pkg;
        REQUIRE(pkg.open(fixture(name)));
        const auto graph = pptctl::PartGraph::build(pkg);

        CHECK(graph.is_reachable("ppt/presentation.xml"));
        CHECK(graph.is_reachable("ppt/slides/slide1.xml"));
        CHECK(graph.is_reachable("ppt/slideMasters/slideMaster1.xml"));
        CHECK(graph.is_reachable("ppt/theme/theme1.xml"));
        CHECK(graph.unreachable().empty());
    }
}

TEST_CASE("PartGraph: package infrastructure is neither reachable nor unreachable") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    CHECK_FALSE(graph.is_reachable("[Content_Types].xml"));
    CHECK_FALSE(graph.is_reachable("_rels/.rels"));
    CHECK_FALSE(graph.is_reachable("ppt/slides/_rels/slide1.xml.rels"));
    CHECK(graph.unreachable().empty());
}

TEST_CASE("PartGraph: walks through notes slides, notes masters and a second master") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);

    CHECK(graph.is_reachable("ppt/notesSlides/notesSlide4.xml"));
    CHECK(graph.is_reachable("ppt/notesMasters/notesMaster1.xml"));
    CHECK(graph.is_reachable("ppt/slideMasters/slideMaster2.xml"));
    CHECK(graph.is_reachable("ppt/theme/theme3.xml"));

    // slide -> notesSlide -> slide is a cycle; the walk must still terminate
    // and record the back edge once.
    const auto back = graph.references_to("ppt/slides/slide1.xml");
    CHECK(std::count_if(back.begin(), back.end(), [](const pptctl::PartReference& r) {
              return r.source == "ppt/notesSlides/notesSlide1.xml";
          }) == 1);
}

TEST_CASE("PartGraph: the root is the source of package-level references") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("three_slides.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    const auto refs = graph.references_to("ppt/presentation.xml");
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].source.empty());
    CHECK(refs[0].rel.target == "ppt/presentation.xml");
}

TEST_CASE("PartGraph: a layout used by every slide has one inbound edge per slide") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);

    // 12 slides use it, and its master lists it with the same rel type.
    const auto refs = graph.references_to("ppt/slideLayouts/slideLayout7.xml");
    std::vector<std::string> sources;
    for (const auto& r : refs) {
        CHECK(r.rel.type == kLayoutType);
        sources.push_back(r.source);
    }
    std::sort(sources.begin(), sources.end());
    REQUIRE(sources.size() == 13);
    CHECK(sources.front() == "ppt/slideMasters/slideMaster1.xml");
    CHECK(sources[1] == "ppt/slides/slide1.xml");
    CHECK(sources.back() == "ppt/slides/slide9.xml");
}

TEST_CASE("PartGraph: media referenced by two slides has two inbound edges") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide2.xml.rels"] = to_bytes(
        std::string(kRelsHead) + rel("rId1", kLayoutType, "../slideLayouts/slideLayout7.xml") +
        rel("rId2", kImageType, "../media/image2.png") +
        rel("rId3", kImageType, "../media/image1.png") + std::string(kRelsTail));
    const auto pkg = images_deck_with(dir, changes);
    const auto graph = pptctl::PartGraph::build(pkg);

    const auto refs = graph.references_to("ppt/media/image1.png");
    REQUIRE(refs.size() == 2);
    std::vector<std::string> sources{refs[0].source, refs[1].source};
    std::sort(sources.begin(), sources.end());
    CHECK(sources == std::vector<std::string>{"ppt/slides/slide1.xml", "ppt/slides/slide2.xml"});
    CHECK(refs[0].rel.type == kImageType);
    CHECK(graph.references_to("ppt/media/image2.png").size() == 1);
}

TEST_CASE("PartGraph: an injected unreferenced part is unreachable") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/orphan.png"] = to_bytes("not really a png");
    const auto pkg = images_deck_with(dir, changes);
    const auto graph = pptctl::PartGraph::build(pkg);

    CHECK_FALSE(graph.is_reachable("ppt/media/orphan.png"));
    CHECK(graph.references_to("ppt/media/orphan.png").empty());
    CHECK(graph.unreachable() == std::vector<std::string>{"ppt/media/orphan.png"});
}

TEST_CASE("PartGraph: references from an unreachable part don't count") {
    // An orphan slide that points at image1: the orphan and its .rels are both
    // unreachable, and image1 keeps only its one live inbound edge.
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide99.xml"] = to_bytes("<p:sld/>");
    changes.replaced["ppt/slides/_rels/slide99.xml.rels"] =
        to_bytes(std::string(kRelsHead) + rel("rId1", kImageType, "../media/image1.png") +
                 std::string(kRelsTail));
    const auto pkg = images_deck_with(dir, changes);
    const auto graph = pptctl::PartGraph::build(pkg);

    CHECK(graph.unreachable() ==
          std::vector<std::string>{"ppt/slides/_rels/slide99.xml.rels", "ppt/slides/slide99.xml"});
    const auto refs = graph.references_to("ppt/media/image1.png");
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].source == "ppt/slides/slide1.xml");
}

TEST_CASE("PartGraph: a dangling target is referenced but not reachable") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide2.xml.rels"] = to_bytes(
        std::string(kRelsHead) + rel("rId1", kLayoutType, "../slideLayouts/slideLayout7.xml") +
        rel("rId2", kImageType, "../media/missing.png") + std::string(kRelsTail));
    const auto pkg = images_deck_with(dir, changes);
    const auto graph = pptctl::PartGraph::build(pkg);

    CHECK_FALSE(graph.is_reachable("ppt/media/missing.png"));
    CHECK(graph.references_to("ppt/media/missing.png").size() == 1);
    // image2 lost its only reference.
    CHECK(graph.unreachable() == std::vector<std::string>{"ppt/media/image2.png"});
}

TEST_CASE("PartGraph: external relationships are not edges") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide2.xml.rels"] = to_bytes(
        std::string(kRelsHead) + rel("rId1", kLayoutType, "../slideLayouts/slideLayout7.xml") +
        rel("rId2", kImageType, "../media/image2.png") +
        R"(<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com/" TargetMode="External"/>)" +
        std::string(kRelsTail));
    const auto pkg = images_deck_with(dir, changes);
    const auto graph = pptctl::PartGraph::build(pkg);

    CHECK_FALSE(graph.is_reachable("https://example.com/"));
    CHECK(graph.references_to("https://example.com/").empty());
    CHECK(graph.unreachable().empty());
}

TEST_CASE("PartGraph: a closed package yields an empty graph") {
    const pptctl::Package pkg;
    const auto graph = pptctl::PartGraph::build(pkg);
    CHECK(graph.reachable().empty());
    CHECK(graph.unreachable().empty());
}
