// Tests for the "layouts" optimize pass (#100): unused slide layouts and masters.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/presentation.hpp>
#include <pugixml.hpp>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
               fs::path("pptctl_layouts_" + run + "_" + std::to_string(counter++));
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

// Replace every occurrence of `from` with `to` in a part's text.
std::string replaced_text(const pptctl::Package& pkg,
                          std::string_view part,
                          std::string_view from,
                          std::string_view to) {
    auto text = to_string(pkg.read_part(part));
    for (auto pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos)) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

constexpr std::string_view kRelsHead =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
    R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
constexpr std::string_view kRelsTail = "</Relationships>";
constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

std::string rel(std::string_view id, std::string_view type, std::string_view target) {
    return std::string(R"(<Relationship Id=")") + std::string(id) + R"(" Type=")" +
           std::string(kRelNs) + std::string(type) + R"(" Target=")" + std::string(target) +
           R"("/>)";
}

// Write `name` with `changes` applied into `dir` and return the path.
std::string variant(const ScratchDir& dir, const char* name, const pptctl::PartChanges& changes) {
    pptctl::Package src;
    REQUIRE(src.open(fixture(name)));
    const auto out = (dir.path / "variant.pptx").string();
    REQUIRE(src.write(out, changes));
    return out;
}

pptctl::PassResult run_pass(const pptctl::Package& pkg) {
    const auto graph = pptctl::PartGraph::build(pkg);
    return pptctl::detail::optimize_layouts(pkg, graph, {});
}

// Only the layouts pass enabled.
pptctl::OptimizeOptions layouts_only() {
    pptctl::OptimizeOptions opts;
    opts.fonts = false;
    opts.crop = false;
    opts.images = false;
    opts.orphans = false;
    return opts;
}

// Run the real pipeline with only the layouts pass on `in`, writing `out`.
pptctl::OptimizeReport optimize_layouts_file(const std::string& in, const std::string& out) {
    std::string error;
    auto report = pptctl::optimize(in, out, layouts_only(), error);
    REQUIRE_MESSAGE(report, error);
    return *report;
}

std::set<std::string> layouts(int from, int to) {
    std::set<std::string> out;
    for (int i = from; i <= to; ++i) {
        out.insert("ppt/slideLayouts/slideLayout" + std::to_string(i) + ".xml");
    }
    return out;
}

// (id, r:id) of each entry of `list` (e.g. "p:sldLayoutIdLst") under `root` in `xml`.
std::vector<std::pair<std::string, std::string>> id_list(const std::string& xml,
                                                         const char* root,
                                                         const char* list) {
    pugi::xml_document doc;
    REQUIRE(doc.load_string(xml.c_str()));
    std::vector<std::pair<std::string, std::string>> out;
    for (auto entry : doc.child(root).child(list).children()) {
        out.emplace_back(entry.attribute("id").as_string(""),
                         entry.attribute("r:id").as_string(""));
    }
    return out;
}

// Every part of `a` that is also in `b` and not in `changed` is byte-identical.
void check_untouched_identical(const pptctl::Package& a,
                               const pptctl::Package& b,
                               const std::set<std::string>& changed) {
    const auto b_names = b.part_names();
    for (const auto& name : a.part_names()) {
        if (changed.contains(name) ||
            std::find(b_names.begin(), b_names.end(), name) == b_names.end()) {
            continue;
        }
        CHECK_MESSAGE(a.read_part(name) == b.read_part(name), name);
    }
}

}  // namespace

// ── the pass itself ───────────────────────────────────────────────────────────

TEST_CASE("optimize layouts: keeps only the used layout, its master and their theme") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    const auto r = run_pass(pkg);

    // Every slide uses slideLayout2 (master 1). Master 2 has only layout 12,
    // which nothing uses, so it goes with its theme (theme3).
    auto expected = layouts(1, 12);
    expected.erase("ppt/slideLayouts/slideLayout2.xml");
    expected.insert("ppt/slideMasters/slideMaster2.xml");
    expected.insert("ppt/theme/theme3.xml");
    CHECK(r.changes.removed == expected);

    // Notes master and its theme, the used master and its theme stay.
    for (const char* kept : {"ppt/slideLayouts/slideLayout2.xml",
                             "ppt/slideMasters/slideMaster1.xml",
                             "ppt/theme/theme1.xml",
                             "ppt/theme/theme2.xml",
                             "ppt/notesMasters/notesMaster1.xml"}) {
        CHECK_FALSE(r.changes.removed.contains(kept));
    }
}

TEST_CASE("optimize layouts: one removed action per removed part, with its size") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    const auto r = run_pass(pkg);

    REQUIRE_FALSE(r.actions.empty());
    REQUIRE(r.actions.size() == r.changes.removed.size());
    std::map<std::string, int> by_detail;
    for (const auto& a : r.actions) {
        CHECK(r.changes.removed.contains(a.part));
        CHECK(a.kind == "removed");
        REQUIRE(pkg.part_size(a.part));
        CHECK(a.bytes_before == *pkg.part_size(a.part));
        CHECK(a.bytes_after == 0);
        ++by_detail[a.detail];
    }
    CHECK(by_detail == std::map<std::string, int>{{"layout", 11}, {"master", 1}, {"theme", 1}});
    // Layouts first, then masters, then what only they used.
    CHECK(r.actions.front().detail == "layout");
    CHECK(r.actions.back().detail == "theme");
}

TEST_CASE("optimize layouts: rewrites the id lists of the surviving master and presentation") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    const auto r = run_pass(pkg);

    // The pass edits part bodies only; .rels and content types are the pipeline's.
    std::set<std::string> replaced;
    for (const auto& [name, bytes] : r.changes.replaced) {
        replaced.insert(name);
    }
    CHECK(replaced ==
          std::set<std::string>{"ppt/presentation.xml", "ppt/slideMasters/slideMaster1.xml"});

    const auto master = to_string(r.changes.replaced.at("ppt/slideMasters/slideMaster1.xml"));
    CHECK(master.starts_with("<?xml"));
    CHECK(id_list(master, "p:sldMaster", "p:sldLayoutIdLst") ==
          std::vector<std::pair<std::string, std::string>>{{"2147483650", "rId2"}});

    const auto pres = to_string(r.changes.replaced.at("ppt/presentation.xml"));
    CHECK(pres.starts_with("<?xml"));
    CHECK(id_list(pres, "p:presentation", "p:sldMasterIdLst") ==
          std::vector<std::pair<std::string, std::string>>{{"2147483648", "rId1"}});
    // Nothing else in presentation.xml moved: the slide list is intact.
    CHECK(id_list(pres, "p:presentation", "p:sldIdLst").size() == 4);
    const auto before = to_string(pkg.read_part("ppt/presentation.xml"));
    CHECK(id_list(pres, "p:presentation", "p:notesMasterIdLst") ==
          id_list(before, "p:presentation", "p:notesMasterIdLst"));
    CHECK(id_list(pres, "p:presentation", "p:notesMasterIdLst").size() == 1);
}

TEST_CASE("optimize layouts: a second master in use survives with its theme, untouched") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("notes_and_masters.pptx")));
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/_rels/slide3.xml.rels"] = to_bytes(replaced_text(
        src, "ppt/slides/_rels/slide3.xml.rels", "slideLayout2.xml", "slideLayout12.xml"));
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "notes_and_masters.pptx", changes)));

    const auto r = run_pass(pkg);
    auto expected = layouts(1, 11);
    expected.erase("ppt/slideLayouts/slideLayout2.xml");
    CHECK(r.changes.removed == expected);
    // No master goes, so presentation.xml isn't rewritten; master 2 lost no layout.
    CHECK_FALSE(r.changes.replaced.contains("ppt/presentation.xml"));
    CHECK_FALSE(r.changes.replaced.contains("ppt/slideMasters/slideMaster2.xml"));
    CHECK(r.changes.replaced.contains("ppt/slideMasters/slideMaster1.xml"));
}

TEST_CASE("optimize layouts: a removed master's theme that is used elsewhere stays") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("notes_and_masters.pptx")));
    pptctl::PartChanges changes;
    for (int i = 1; i <= 4; ++i) {
        const auto rels = "ppt/slides/_rels/slide" + std::to_string(i) + ".xml.rels";
        changes.replaced[rels] =
            to_bytes(replaced_text(src, rels, "slideLayout2.xml", "slideLayout12.xml"));
    }
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "notes_and_masters.pptx", changes)));

    // Master 1 and all its layouts go; theme1 is also the presentation's own
    // theme (ppt/_rels/presentation.xml.rels), so it stays.
    const auto r = run_pass(pkg);
    auto expected = layouts(1, 11);
    expected.insert("ppt/slideMasters/slideMaster1.xml");
    CHECK(r.changes.removed == expected);
    CHECK_FALSE(r.changes.replaced.contains("ppt/slideMasters/slideMaster2.xml"));
    const auto pres = to_string(r.changes.replaced.at("ppt/presentation.xml"));
    CHECK(id_list(pres, "p:presentation", "p:sldMasterIdLst") ==
          std::vector<std::pair<std::string, std::string>>{{"2147483700", "rId12"}});
}

TEST_CASE("optimize layouts: media only a removed layout used goes with it; shared media stays") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    pptctl::PartChanges changes;
    changes.replaced["ppt/slideLayouts/_rels/slideLayout3.xml.rels"] = to_bytes(
        std::string(kRelsHead) + rel("rId1", "slideMaster", "../slideMasters/slideMaster1.xml") +
        rel("rId2", "image", "../media/image1.png") +
        rel("rId3", "image", "../media/layout_only.png") + std::string(kRelsTail));
    changes.replaced["ppt/media/layout_only.png"] = src.read_part("ppt/media/image2.png");
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "images_deck.pptx", changes)));

    const auto r = run_pass(pkg);
    CHECK(r.changes.removed.contains("ppt/slideLayouts/slideLayout3.xml"));
    CHECK(r.changes.removed.contains("ppt/media/layout_only.png"));
    CHECK_FALSE(r.changes.removed.contains("ppt/media/image1.png"));
    const auto it = std::find_if(r.actions.begin(), r.actions.end(), [](const auto& a) {
        return a.part == "ppt/media/layout_only.png";
    });
    REQUIRE(it != r.actions.end());
    CHECK(it->detail == "media");
}

TEST_CASE("optimize layouts: a deck with no slides keeps its first master and first layout") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("minimal.pptx")));
    pptctl::PartChanges changes;
    auto pres = to_string(src.read_part("ppt/presentation.xml"));
    const auto lst = pres.find("<p:sldIdLst>");
    const auto end = pres.find("</p:sldIdLst>");
    REQUIRE(lst != std::string::npos);
    pres.erase(lst, end + std::string_view("</p:sldIdLst>").size() - lst);
    changes.replaced["ppt/presentation.xml"] = to_bytes(pres);
    changes.replaced["ppt/_rels/presentation.xml.rels"] =
        pptctl::rewrite_rels(src.read_part("ppt/_rels/presentation.xml.rels"),
                             "ppt/presentation.xml",
                             {"ppt/slides/slide1.xml"});
    changes.removed = {"ppt/slides/slide1.xml", "ppt/slides/_rels/slide1.xml.rels"};
    const auto in = variant(dir, "minimal.pptx", changes);

    pptctl::Package pkg;
    REQUIRE(pkg.open(in));
    const auto r = run_pass(pkg);
    CHECK(r.changes.removed == layouts(2, 11));
    CHECK_FALSE(r.changes.replaced.contains("ppt/presentation.xml"));
    const auto master = to_string(r.changes.replaced.at("ppt/slideMasters/slideMaster1.xml"));
    CHECK(id_list(master, "p:sldMaster", "p:sldLayoutIdLst") ==
          std::vector<std::pair<std::string, std::string>>{{"2147483649", "rId1"}});

    const auto out = (dir.path / "out.pptx").string();
    (void)optimize_layouts_file(in, out);
    pptctl::Package result;
    REQUIRE(result.open(out));
    pptctl::Presentation p(result);
    REQUIRE(p.load());
    CHECK(p.slide_count() == 0);
    CHECK(p.master_count() == 1);
    CHECK(p.layout_count() == 1);
    CHECK(pptctl::PartGraph::build(result).unreachable().empty());
}

TEST_CASE("optimize layouts: an unreachable slide doesn't keep its layout alive") {
    ScratchDir dir;
    pptctl::Package src;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    pptctl::PartChanges changes;
    changes.replaced["ppt/slides/slide99.xml"] = src.read_part("ppt/slides/slide1.xml");
    changes.replaced["ppt/slides/_rels/slide99.xml.rels"] = to_bytes(
        std::string(kRelsHead) + rel("rId1", "slideLayout", "../slideLayouts/slideLayout5.xml") +
        std::string(kRelsTail));
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "images_deck.pptx", changes)));

    const auto r = run_pass(pkg);
    auto expected = layouts(1, 11);
    expected.erase("ppt/slideLayouts/slideLayout7.xml");
    CHECK(r.changes.removed == expected);
}

TEST_CASE("optimize layouts: an unparseable presentation.xml means no changes") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/presentation.xml"] = to_bytes("<p:presentation><p:sldMasterIdLst>");
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "notes_and_masters.pptx", changes)));

    const auto r = run_pass(pkg);
    CHECK(r.changes.removed.empty());
    CHECK(r.changes.replaced.empty());
    CHECK(r.actions.empty());
}

TEST_CASE("optimize layouts: an unparseable master means no changes") {
    ScratchDir dir;
    pptctl::PartChanges changes;
    changes.replaced["ppt/slideMasters/slideMaster1.xml"] = to_bytes("<p:sldMaster>");
    pptctl::Package pkg;
    REQUIRE(pkg.open(variant(dir, "images_deck.pptx", changes)));

    const auto r = run_pass(pkg);
    CHECK(r.changes.removed.empty());
    CHECK(r.changes.replaced.empty());
    CHECK(r.actions.empty());
}

// ── end to end through the pipeline ───────────────────────────────────────────

TEST_CASE("optimize layouts: notes_and_masters end to end") {
    ScratchDir dir;
    const auto in = fixture("notes_and_masters.pptx");
    const auto out = (dir.path / "out.pptx").string();
    const auto report = optimize_layouts_file(in, out);

    REQUIRE(report.passes.size() == 1);
    CHECK(report.passes[0].id == "layouts");
    CHECK(report.passes[0].actions.size() == 13);
    CHECK(report.output_bytes < report.input_bytes);

    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(in));
    REQUIRE(b.open(out));
    pptctl::Presentation before(a);
    pptctl::Presentation after(b);
    REQUIRE(before.load());
    REQUIRE(after.load());
    CHECK(before.master_count() == 2);
    CHECK(before.layout_count() == 12);
    CHECK(after.master_count() == 1);
    CHECK(after.layout_count() == 1);
    CHECK(after.slide_count() == 4);
    CHECK(after.slide_parts() == before.slide_parts());

    const auto graph = pptctl::PartGraph::build(b);
    CHECK(graph.unreachable().empty());
    CHECK(graph.is_reachable("ppt/notesMasters/notesMaster1.xml"));
    CHECK(graph.is_reachable("ppt/theme/theme2.xml"));
    CHECK_FALSE(b.part_size("ppt/slideMasters/_rels/slideMaster2.xml.rels"));
    CHECK_FALSE(b.part_size("ppt/slideLayouts/_rels/slideLayout1.xml.rels"));

    // The surviving master's rels and id list agree.
    const auto rels = b.relationships("ppt/slideMasters/slideMaster1.xml");
    REQUIRE(rels.size() == 2);
    CHECK(b.resolve("ppt/slideMasters/slideMaster1.xml", "rId2") ==
          "ppt/slideLayouts/slideLayout2.xml");
    CHECK(b.resolve("ppt/presentation.xml", "rId1") == "ppt/slideMasters/slideMaster1.xml");
    CHECK(b.resolve("ppt/presentation.xml", "rId12").empty());
    const auto types = to_string(b.read_part("[Content_Types].xml"));
    CHECK(types.find("slideMaster2.xml") == std::string::npos);
    CHECK(types.find("slideLayout12.xml") == std::string::npos);
    CHECK(types.find("theme3.xml") == std::string::npos);

    // Everything the pass had no reason to touch is byte-identical.
    check_untouched_identical(a,
                              b,
                              {"ppt/presentation.xml",
                               "ppt/_rels/presentation.xml.rels",
                               "ppt/slideMasters/slideMaster1.xml",
                               "ppt/slideMasters/_rels/slideMaster1.xml.rels",
                               "[Content_Types].xml"});
}

TEST_CASE("optimize layouts: single-master decks keep their master and used layouts") {
    for (const char* name : {"images_deck.pptx", "minimal.pptx", "three_slides.pptx"}) {
        CAPTURE(name);
        ScratchDir dir;
        const auto in = fixture(name);
        const auto out = (dir.path / "out.pptx").string();
        (void)optimize_layouts_file(in, out);

        pptctl::Package a;
        pptctl::Package b;
        REQUIRE(a.open(in));
        REQUIRE(b.open(out));
        pptctl::Presentation before(a);
        pptctl::Presentation after(b);
        REQUIRE(before.load());
        REQUIRE(after.load());
        CHECK(after.master_count() == 1);
        CHECK(after.slide_parts() == before.slide_parts());
        // Each of these decks uses exactly one layout of its eleven.
        CHECK(before.layout_count() == 11);
        CHECK(after.layout_count() == 1);
        CHECK(pptctl::PartGraph::build(b).unreachable().empty());
        for (const auto& slide : before.slide_parts()) {
            CHECK(a.read_part(slide) == b.read_part(slide));
            CHECK(a.read_part(pptctl::rels_part_for(slide)) ==
                  b.read_part(pptctl::rels_part_for(slide)));
        }
    }
}

TEST_CASE("optimize layouts: a second run has nothing to do") {
    ScratchDir dir;
    const auto once = (dir.path / "once.pptx").string();
    (void)optimize_layouts_file(fixture("notes_and_masters.pptx"), once);
    pptctl::Package pkg;
    REQUIRE(pkg.open(once));
    const auto r = run_pass(pkg);
    CHECK(r.changes.removed.empty());
    CHECK(r.changes.replaced.empty());
    CHECK(r.actions.empty());
}
