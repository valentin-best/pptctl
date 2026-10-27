// Tests for the "orphans" optimize pass (#99): media nothing live references.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pugixml.hpp>
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
               fs::path("pptctl_orphans_" + run + "_" + std::to_string(counter++));
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

std::string to_string(const std::vector<std::byte>& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
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

std::vector<std::byte> rels(const std::vector<std::string>& entries) {
    std::string xml(kRelsHead);
    for (const auto& e : entries) {
        xml += e;
    }
    xml += kRelsTail;
    return to_bytes(xml);
}

// Write fixture `name` with `changes` applied to `out` (in `dir`).
std::string write_variant(const ScratchDir& dir,
                          const char* name,
                          const pptctl::PartChanges& changes,
                          const char* out = "in.pptx") {
    pptctl::Package src;
    REQUIRE(src.open(fixture(name)));
    const auto path = (dir.path / out).string();
    REQUIRE(src.write(path, changes));
    return path;
}

// Run the orphans pass over `pkg`.
pptctl::PassResult run_pass(const pptctl::Package& pkg) {
    const auto graph = pptctl::PartGraph::build(pkg);
    return pptctl::detail::optimize_orphans(pkg, graph, {});
}

pptctl::PassResult run_on_variant(const char* name, const pptctl::PartChanges& changes) {
    ScratchDir dir;
    pptctl::Package pkg;
    REQUIRE(pkg.open(write_variant(dir, name, changes)));
    return run_pass(pkg);
}

std::vector<std::string> action_parts(const pptctl::PassResult& r) {
    std::vector<std::string> parts;
    for (const auto& a : r.actions) {
        parts.push_back(a.part);
    }
    return parts;
}

}  // namespace

TEST_CASE("optimize orphans: a well-formed deck has no orphan media") {
    for (const char* name : {"minimal.pptx",
                             "three_slides.pptx",
                             "images_deck.pptx",
                             "notes_and_masters.pptx",
                             "bloat_deck.pptx",
                             "charts_deck.pptx"}) {
        CAPTURE(name);
        pptctl::Package pkg;
        REQUIRE(pkg.open(fixture(name)));
        const auto result = run_pass(pkg);
        CHECK(result.changes.removed.empty());
        CHECK(result.changes.replaced.empty());
        CHECK(result.actions.empty());
    }
}

TEST_CASE("optimize orphans: an injected unreferenced image is removed and reported") {
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/orphan.png"] = to_bytes("not really a png");
    const auto result = run_on_variant("images_deck.pptx", changes);

    CHECK(result.changes.removed == std::set<std::string>{"ppt/media/orphan.png"});
    CHECK(result.changes.replaced.empty());
    REQUIRE(result.actions.size() == 1);
    CHECK(result.actions[0].part == "ppt/media/orphan.png");
    CHECK(result.actions[0].kind == "removed");
    CHECK(result.actions[0].bytes_before == 16);
    CHECK(result.actions[0].bytes_after == 0);
}

TEST_CASE("optimize orphans: referenced media is untouched") {
    // image1 gains a second reference and image2 loses its only one: image1
    // stays, image2 goes, and every other image stays.
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide2.xml.rels"] =
        rels({rel("rId1", kLayoutType, "../slideLayouts/slideLayout7.xml"),
              rel("rId2", kImageType, "../media/image1.png")});
    const auto result = run_on_variant("images_deck.pptx", changes);

    CHECK(result.changes.removed == std::set<std::string>{"ppt/media/image2.png"});
    CHECK(action_parts(result) == std::vector<std::string>{"ppt/media/image2.png"});
}

TEST_CASE("optimize orphans: media referenced only from an unreachable part is removed") {
    // An orphan slide points at a new image and at image1. The new image has
    // no live reference and goes; image1 is still used by slide1. The orphan
    // slide itself is not media and stays.
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/image99.png"] = to_bytes("only the dead slide uses me");
    changes.replaced["ppt/slides/slide99.xml"] = to_bytes("<p:sld/>");
    changes.replaced["ppt/slides/_rels/slide99.xml.rels"] =
        rels({rel("rId1", kImageType, "../media/image99.png"),
              rel("rId2", kImageType, "../media/image1.png")});
    const auto result = run_on_variant("images_deck.pptx", changes);

    CHECK(result.changes.removed == std::set<std::string>{"ppt/media/image99.png"});
    CHECK(action_parts(result) == std::vector<std::string>{"ppt/media/image99.png"});
}

TEST_CASE("optimize orphans: nothing outside ppt/media/ is removed, even if unreachable") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide99.xml"] = to_bytes("<p:sld/>");
    changes.replaced["ppt/embeddings/oleObject9.bin"] = to_bytes("ole");
    changes.replaced["ppt/fonts/font9.fntdata"] = to_bytes("font");
    changes.replaced["customXml/item9.xml"] = to_bytes("<x/>");
    changes.replaced["media/image1.png"] = to_bytes("not under ppt/");
    changes.replaced["ppt/mediaextra/image1.png"] = to_bytes("prefix, not the folder");
    changes.replaced["ppt/slides/media/image1.png"] = to_bytes("nested, not the folder");
    pptctl::Package pkg;
    REQUIRE(pkg.open(write_variant(dir, "images_deck.pptx", changes)));
    const auto graph = pptctl::PartGraph::build(pkg);
    REQUIRE(graph.unreachable().size() == changes.replaced.size());

    const auto result = pptctl::detail::optimize_orphans(pkg, graph, {});
    CHECK(result.changes.removed.empty());
    CHECK(result.actions.empty());
}

TEST_CASE("optimize orphans: a media part's own .rels is left to the pipeline") {
    // complete_removals drops a removed part's .rels, so the pass lists only
    // the media part itself.
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/orphan.png"] = to_bytes("png");
    changes.replaced["ppt/media/_rels/orphan.png.rels"] =
        rels({rel("rId1", kImageType, "image1.png")});
    pptctl::Package pkg;
    REQUIRE(pkg.open(write_variant(dir, "images_deck.pptx", changes)));
    const auto graph = pptctl::PartGraph::build(pkg);

    const auto result = pptctl::detail::optimize_orphans(pkg, graph, {});
    CHECK(result.changes.removed == std::set<std::string>{"ppt/media/orphan.png"});
    CHECK(action_parts(result) == std::vector<std::string>{"ppt/media/orphan.png"});

    const auto done = pptctl::complete_removals(pkg, graph, result.changes);
    CHECK(done.removed ==
          std::set<std::string>{"ppt/media/orphan.png", "ppt/media/_rels/orphan.png.rels"});
}

TEST_CASE("optimize orphans: a reference differing only in case or %-encoding keeps the part") {
    // OPC part names compare case-insensitively and targets may be
    // percent-encoded. The graph matches names exactly, so these parts look
    // unreachable, but renderers would still resolve the references to them.
    pptctl::PartChanges changes;
    changes.removed.insert("ppt/media/image2.png");
    changes.replaced["ppt/media/Image2.PNG"] = to_bytes("slide2 still points here");
    changes.replaced["ppt/media/my pic.png"] = to_bytes("slide3 points here, encoded");
    changes.replaced["ppt/slides/_rels/slide3.xml.rels"] =
        rels({rel("rId1", kLayoutType, "../slideLayouts/slideLayout7.xml"),
              rel("rId2", kImageType, "../media/my%20pic.png")});
    // Only a dead slide uses this one, whatever the spelling: it still goes.
    changes.replaced["ppt/media/Dead.png"] = to_bytes("dead");
    changes.replaced["ppt/slides/slide99.xml"] = to_bytes("<p:sld/>");
    changes.replaced["ppt/slides/_rels/slide99.xml.rels"] =
        rels({rel("rId1", kImageType, "../media/dead.png")});
    ScratchDir dir;
    pptctl::Package pkg;
    REQUIRE(pkg.open(write_variant(dir, "images_deck.pptx", changes)));
    const auto graph = pptctl::PartGraph::build(pkg);
    REQUIRE_FALSE(graph.is_reachable("ppt/media/Image2.PNG"));
    REQUIRE_FALSE(graph.is_reachable("ppt/media/my pic.png"));

    const auto result = pptctl::detail::optimize_orphans(pkg, graph, {});
    // image3 lost its reference from slide3.
    CHECK(result.changes.removed ==
          std::set<std::string>{"ppt/media/Dead.png", "ppt/media/image3.png"});
}

TEST_CASE("optimize orphans: end to end, with only the orphans pass enabled") {
    // Two orphans: a PNG covered by the png Default, and a part whose content
    // type comes from an Override that must go with it.
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/orphan.png"] = to_bytes("orphan png bytes");
    changes.replaced["ppt/media/hdphoto1.wdp"] = to_bytes("orphan wdp");
    changes.replaced["[Content_Types].xml"] =
        pptctl::rewrite_content_types(src.read_part("[Content_Types].xml"),
                                      {},
                                      {{"ppt/media/hdphoto1.wdp", "image/vnd.ms-photo"}});
    const auto in = write_variant(dir, "images_deck.pptx", changes);
    const auto out = (dir.path / "out.pptx").string();

    pptctl::Package before;
    REQUIRE(before.open(in));
    REQUIRE(before.content_type("ppt/media/hdphoto1.wdp") == "image/vnd.ms-photo");

    const pptctl::OptimizeOptions opts{
        .layouts = false, .fonts = false, .crop = false, .images = false, .orphans = true};
    std::string error;
    const auto report = pptctl::optimize(in, out, opts, error);
    REQUIRE_MESSAGE(report, error);
    REQUIRE(report->passes.size() == 1);
    CHECK(report->passes[0].id == "orphans");
    std::map<std::string, std::uint64_t> reported;
    for (const auto& a : report->passes[0].actions) {
        CHECK(a.kind == "removed");
        CHECK(a.bytes_after == 0);
        reported[a.part] = a.bytes_before;
    }
    CHECK(reported == std::map<std::string, std::uint64_t>{{"ppt/media/hdphoto1.wdp", 10},
                                                           {"ppt/media/orphan.png", 16}});
    CHECK(report->output_bytes < report->input_bytes);

    pptctl::Package after;
    REQUIRE(after.open(out));
    auto expected = before.part_names();
    std::erase_if(expected, [](const std::string& n) {
        return n == "ppt/media/orphan.png" || n == "ppt/media/hdphoto1.wdp";
    });
    CHECK(after.part_names() == expected);
    CHECK(pptctl::PartGraph::build(after).unreachable().empty());

    // Content types: still well-formed, the Override gone, the Defaults kept.
    const auto types = after.read_part("[Content_Types].xml");
    pugi::xml_document doc;
    REQUIRE(doc.load_buffer(types.data(), types.size()));
    REQUIRE(doc.child("Types"));
    CHECK(to_string(types).find("hdphoto1") == std::string::npos);
    CHECK(to_string(types).find(R"(Extension="png")") != std::string::npos);
    for (const auto& name : after.part_names()) {
        CAPTURE(name);
        CHECK_FALSE(after.content_type(name).empty());
    }

    // Everything else is byte-identical; content types only lost the Override.
    for (const auto& name : after.part_names()) {
        if (name == "[Content_Types].xml") {
            continue;
        }
        CAPTURE(name);
        CHECK(after.read_part(name) == before.read_part(name));
    }
    CHECK(to_string(types).size() < to_string(before.read_part("[Content_Types].xml")).size());
}
