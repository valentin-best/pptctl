// Tests for the optimize pipeline, complete_removals and the optimize command.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace pptctl::cmd {
int optimize_to(const std::string& in,
                const OptimizeOptions& opts,
                const OptimizeOutputOptions& out,
                std::ostream& os);
void render_optimize(const OptimizeReport& report,
                     const std::string& in,
                     const OptimizeOutputOptions& out,
                     std::ostream& os);
}  // namespace pptctl::cmd

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
               fs::path("pptctl_optimize_" + run + "_" + std::to_string(counter++));
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

bool has(const std::vector<std::string>& names, std::string_view n) {
    return std::find(names.begin(), names.end(), n) != names.end();
}

// A test pass that removes one part and reports it, touching nothing else.
pptctl::RegisteredPass removing(std::string id, std::string part) {
    return {
        std::move(id),
        &pptctl::OptimizeOptions::orphans,
        [part](
            const pptctl::Package& pkg, const pptctl::PartGraph&, const pptctl::OptimizeOptions&) {
            pptctl::PassResult r;
            if (pkg.part_size(part)) {
                r.changes.removed.insert(part);
                r.actions.push_back({part, "removed", "", *pkg.part_size(part), 0});
            }
            return r;
        }};
}

}  // namespace

// ── registry ──────────────────────────────────────────────────────────────────

TEST_CASE("optimize_passes: the built-in passes, in run order") {
    std::vector<std::string> ids;
    for (const auto& p : pptctl::optimize_passes()) {
        ids.push_back(p.id);
        CHECK(p.run);
    }
    CHECK(ids == std::vector<std::string>{"layouts", "fonts", "crop", "images", "orphans"});
}

// ── complete_removals ─────────────────────────────────────────────────────────

TEST_CASE("complete_removals: drops the relationships that target a removed part") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    pptctl::PartChanges changes;
    changes.removed.insert("ppt/media/image2.png");

    const auto done = pptctl::complete_removals(pkg, graph, changes);
    CHECK(done.removed == std::set<std::string>{"ppt/media/image2.png"});
    REQUIRE(done.replaced.contains("ppt/slides/_rels/slide2.xml.rels"));
    const auto rels = to_string(done.replaced.at("ppt/slides/_rels/slide2.xml.rels"));
    CHECK(rels.find("image2.png") == std::string::npos);
    CHECK(rels.find("slideLayout7.xml") != std::string::npos);
    // PNG is covered by a Default, so content types have nothing to drop.
    CHECK_FALSE(done.replaced.contains("[Content_Types].xml"));
    CHECK(done.replaced.size() == 1);
}

TEST_CASE("complete_removals: removes a removed part's own .rels and its Override") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("notes_and_masters.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    pptctl::PartChanges changes;
    changes.removed.insert("ppt/notesSlides/notesSlide2.xml");

    const auto done = pptctl::complete_removals(pkg, graph, changes);
    CHECK(done.removed.contains("ppt/notesSlides/_rels/notesSlide2.xml.rels"));
    REQUIRE(done.replaced.contains("[Content_Types].xml"));
    const auto ct = to_string(done.replaced.at("[Content_Types].xml"));
    CHECK(ct.find("/ppt/notesSlides/notesSlide2.xml") == std::string::npos);
    CHECK(ct.find("/ppt/notesSlides/notesSlide1.xml") != std::string::npos);
    REQUIRE(done.replaced.contains("ppt/slides/_rels/slide2.xml.rels"));
    CHECK(to_string(done.replaced.at("ppt/slides/_rels/slide2.xml.rels")).find("notesSlide2") ==
          std::string::npos);
    // notesSlide2's own .rels pointed back at slide2; it's removed, not rewritten.
    CHECK_FALSE(done.replaced.contains("ppt/notesSlides/_rels/notesSlide2.xml.rels"));
}

TEST_CASE("complete_removals: rewrites the pass's own replacement of a .rels") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    pptctl::PartChanges changes;
    changes.removed.insert("ppt/media/image2.png");
    const std::string mine =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
        R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout7.xml"/>)"
        R"(<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/image2.png"/>)"
        R"(<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/image1.png"/>)"
        R"(</Relationships>)";
    changes.replaced["ppt/slides/_rels/slide2.xml.rels"] = to_bytes(mine);

    const auto done = pptctl::complete_removals(pkg, graph, changes);
    const auto rels = to_string(done.replaced.at("ppt/slides/_rels/slide2.xml.rels"));
    CHECK(rels.find("image2.png") == std::string::npos);
    CHECK(rels.find(R"(Id="rId9")") != std::string::npos);
}

TEST_CASE("complete_removals: nothing removed means nothing added") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(fixture("images_deck.pptx")));
    const auto graph = pptctl::PartGraph::build(pkg);
    pptctl::PartChanges changes;
    changes.replaced["ppt/media/image1.png"] = to_bytes("smaller");
    const auto done = pptctl::complete_removals(pkg, graph, changes);
    CHECK(done.removed.empty());
    CHECK(done.replaced.size() == 1);
}

// ── pipeline ──────────────────────────────────────────────────────────────────

TEST_CASE("optimize: passes that change nothing leave every part byte-identical") {
    ScratchDir dir;
    const auto in = fixture("images_deck.pptx");
    const auto out = (dir.path / "out.pptx").string();
    const auto noop = [](const pptctl::Package&,
                         const pptctl::PartGraph&,
                         const pptctl::OptimizeOptions&) { return pptctl::PassResult{}; };
    const std::vector<pptctl::RegisteredPass> passes{
        {"layouts", &pptctl::OptimizeOptions::layouts, noop},
        {"orphans", &pptctl::OptimizeOptions::orphans, noop},
    };
    std::string error;
    const auto report = pptctl::optimize(in, out, {}, error, passes);
    REQUIRE_MESSAGE(report, error);

    REQUIRE(report->passes.size() == 2);
    for (const auto& p : report->passes) {
        CHECK(p.actions.empty());
    }
    CHECK(report->input_bytes == fs::file_size(in));
    CHECK(report->output_bytes == fs::file_size(out));

    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(in));
    REQUIRE(b.open(out));
    CHECK(a.part_names() == b.part_names());
    for (const auto& part : a.part_names()) {
        CAPTURE(part);
        CHECK(a.read_part(part) == b.read_part(part));
    }
}

TEST_CASE("optimize: a disabled pass doesn't run and isn't reported") {
    ScratchDir dir;
    bool ran = false;
    const std::vector<pptctl::RegisteredPass> passes{
        {"fonts",
         &pptctl::OptimizeOptions::fonts,
         [&ran](const pptctl::Package&, const pptctl::PartGraph&, const pptctl::OptimizeOptions&) {
             ran = true;
             return pptctl::PassResult{};
         }},
        removing("orphans", "ppt/media/image2.png"),
    };
    pptctl::OptimizeOptions opts;
    opts.fonts = false;
    std::string error;
    const auto report = pptctl::optimize(
        fixture("images_deck.pptx"), (dir.path / "out.pptx").string(), opts, error, passes);
    REQUIRE(report);
    CHECK_FALSE(ran);
    REQUIRE(report->passes.size() == 1);
    CHECK(report->passes[0].id == "orphans");
}

TEST_CASE("optimize: a removal is completed and written; later passes see it") {
    ScratchDir dir;
    bool saw_removal = false;
    const std::vector<pptctl::RegisteredPass> passes{
        removing("layouts", "ppt/media/image2.png"),
        {"orphans",
         &pptctl::OptimizeOptions::orphans,
         [&saw_removal](const pptctl::Package& pkg,
                        const pptctl::PartGraph& graph,
                        const pptctl::OptimizeOptions&) {
             saw_removal = !pkg.part_size("ppt/media/image2.png") &&
                           graph.references_to("ppt/media/image2.png").empty() &&
                           graph.unreachable().empty();
             return pptctl::PassResult{};
         }},
    };
    const auto out = (dir.path / "out.pptx").string();
    std::string error;
    const auto report = pptctl::optimize(fixture("images_deck.pptx"), out, {}, error, passes);
    REQUIRE_MESSAGE(report, error);
    CHECK(saw_removal);

    REQUIRE(report->passes.size() == 2);
    REQUIRE(report->passes[0].actions.size() == 1);
    CHECK(report->passes[0].actions[0].part == "ppt/media/image2.png");
    CHECK(report->output_bytes == fs::file_size(out));
    CHECK(report->output_bytes < report->input_bytes);

    pptctl::Package src;
    pptctl::Package dst;
    REQUIRE(src.open(fixture("images_deck.pptx")));
    REQUIRE(dst.open(out));
    CHECK_FALSE(has(dst.part_names(), "ppt/media/image2.png"));
    CHECK(pptctl::PartGraph::build(dst).unreachable().empty());
    // Untouched parts stay byte-identical.
    CHECK(dst.read_part("ppt/slides/_rels/slide1.xml.rels") ==
          src.read_part("ppt/slides/_rels/slide1.xml.rels"));
}

TEST_CASE("optimize: a dry run writes nothing but still reports the output size") {
    ScratchDir dir;
    const std::vector<pptctl::RegisteredPass> passes{removing("orphans", "ppt/media/image2.png")};
    std::string error;
    const auto report = pptctl::optimize(fixture("images_deck.pptx"), "", {}, error, passes);
    REQUIRE_MESSAGE(report, error);
    CHECK(report->output_bytes > 0);
    CHECK(report->output_bytes < report->input_bytes);
    CHECK(fs::is_empty(dir.path));
}

TEST_CASE("optimize: an unreadable input or a failed write is an error") {
    ScratchDir dir;
    std::string error;
    CHECK_FALSE(pptctl::optimize(fixture("nope.pptx"), (dir.path / "o.pptx").string(), {}, error));
    CHECK_FALSE(error.empty());

    error.clear();
    const auto missing_dir = (dir.path / "no" / "such" / "o.pptx").string();
    CHECK_FALSE(pptctl::optimize(fixture("images_deck.pptx"), missing_dir, {}, error));
    CHECK_FALSE(error.empty());
}

// ── command ───────────────────────────────────────────────────────────────────

TEST_CASE("optimize command: an output is required unless --in-place or --dry-run") {
    std::ostringstream os;
    CHECK(pptctl::cmd::optimize_to(fixture("images_deck.pptx"), {}, {}, os) == 1);

    pptctl::OptimizeOutputOptions both;
    both.output = "x.pptx";
    both.in_place = true;
    CHECK(pptctl::cmd::optimize_to(fixture("images_deck.pptx"), {}, both, os) == 1);
}

TEST_CASE("optimize command: -o naming the input requires --in-place") {
    ScratchDir dir;
    const auto copy = dir.path / "deck.pptx";
    fs::copy_file(fixture("images_deck.pptx"), copy);
    pptctl::OptimizeOutputOptions out;
    out.output = (dir.path / "." / "deck.pptx").string();
    std::ostringstream os;
    CHECK(pptctl::cmd::optimize_to(copy.string(), {}, out, os) == 1);
}

TEST_CASE("optimize command: --in-place rewrites the input") {
    ScratchDir dir;
    const auto copy = dir.path / "deck.pptx";
    fs::copy_file(fixture("images_deck.pptx"), copy);
    pptctl::OptimizeOutputOptions out;
    out.in_place = true;
    std::ostringstream os;
    CHECK(pptctl::cmd::optimize_to(copy.string(), {}, out, os) == 0);
    pptctl::Package pkg;
    CHECK(pkg.open(copy.string()));
    // No temp file left next to it.
    CHECK(std::distance(fs::directory_iterator(dir.path), fs::directory_iterator{}) == 1);
}

TEST_CASE("optimize command: --json reports every enabled pass") {
    ScratchDir dir;
    pptctl::OptimizeOutputOptions out;
    out.output = (dir.path / "out.pptx").string();
    out.json = true;
    pptctl::OptimizeOptions opts;
    opts.fonts = false;
    std::ostringstream os;
    REQUIRE(pptctl::cmd::optimize_to(fixture("images_deck.pptx"), opts, out, os) == 0);

    const auto j = nlohmann::json::parse(os.str());
    CHECK(j["schema_version"] == 1);
    CHECK(j["output"] == out.output);
    CHECK(j["dry_run"] == false);
    CHECK(j["input_bytes"] == fs::file_size(fixture("images_deck.pptx")));
    CHECK(j["output_bytes"] == fs::file_size(out.output));
    std::vector<std::string> ids;
    for (const auto& p : j["passes"]) {
        ids.push_back(p["id"]);
        CHECK(p["actions"].is_array());
    }
    CHECK(ids == std::vector<std::string>{"layouts", "crop", "images", "orphans"});
}

TEST_CASE("optimize command: --dry-run writes nothing and says so") {
    ScratchDir dir;
    pptctl::OptimizeOutputOptions out;
    out.output = (dir.path / "out.pptx").string();
    out.dry_run = true;
    std::ostringstream os;
    CHECK(pptctl::cmd::optimize_to(fixture("images_deck.pptx"), {}, out, os) == 0);
    CHECK_FALSE(fs::exists(out.output));
    CHECK(os.str().find("dry run") != std::string::npos);
}

TEST_CASE("optimize command: the summary groups each pass's actions by kind") {
    pptctl::OptimizeReport report;
    report.input_bytes = 40'000'000;
    report.output_bytes = 10'000'000;
    report.passes.push_back(
        {"images",
         {{"ppt/media/a.jpeg", "recompressed", "", 20'000'000, 4'000'000},
          {"ppt/media/b.png", "downsampled", "4000x3000 -> 1920x1440", 10'000'000, 2'000'000},
          {"ppt/media/c.jpeg", "recompressed", "", 9'000'000, 3'000'000}}});
    report.passes.push_back({"fonts", {}});
    pptctl::OptimizeOutputOptions out;
    out.output = "out.pptx";
    std::ostringstream os;
    pptctl::cmd::render_optimize(report, "in.pptx", out, os);
    const auto text = os.str();
    CAPTURE(text);
    CHECK(text.find("images: 2 recompressed, 1 downsampled (4000x3000 -> 1920x1440), 37.2 MiB -> "
                    "8.6 MiB") != std::string::npos);
    CHECK(text.find("fonts: nothing to do") != std::string::npos);
    CHECK(text.find("total: 38.1 MiB -> 9.5 MiB (-75%)") != std::string::npos);
}

TEST_CASE("optimize command: a change under 1% isn't shown as +0%") {
    pptctl::OptimizeOutputOptions out;
    out.output = "out.pptx";
    for (const auto& [after, expected] : std::vector<std::pair<std::uint64_t, std::string>>{
             {996, "(-<1%)"}, {1'004, "(+<1%)"}, {1'000, "(no change)"}, {980, "(-2%)"}}) {
        pptctl::OptimizeReport report;
        report.input_bytes = 1'000;
        report.output_bytes = after;
        std::ostringstream os;
        pptctl::cmd::render_optimize(report, "in.pptx", out, os);
        CAPTURE(os.str());
        CHECK(os.str().find(expected) != std::string::npos);
    }
}

TEST_CASE("optimize command: the summary lists each kind's details") {
    pptctl::OptimizeReport report;
    report.passes.push_back({"fonts",
                             {{"ppt/fonts/font1.fntdata", "removed", "Lato", 10, 0},
                              {"ppt/fonts/font2.fntdata", "removed", "Lato Bold", 10, 0}}});
    std::vector<pptctl::OptimizeAction> layouts;
    for (int i = 0; i < 11; ++i) {
        layouts.push_back({"l", "removed", "layout", 1, 0});
    }
    layouts.push_back({"m", "removed", "master", 1, 0});
    layouts.push_back({"t", "removed", "theme", 1, 0});
    report.passes.push_back({"layouts", layouts});
    std::vector<pptctl::OptimizeAction> images;
    for (int i = 0; i < 6; ++i) {
        images.push_back({"i", "downsampled", std::format("{}x10 -> 1x1", i + 1), 1, 0});
    }
    images.push_back({"j", "recompressed", "", 1, 0});
    report.passes.push_back({"images", images});

    pptctl::OptimizeOutputOptions out;
    out.output = "out.pptx";
    std::ostringstream os;
    pptctl::cmd::render_optimize(report, "in.pptx", out, os);
    const auto text = os.str();
    CAPTURE(text);
    CHECK(text.find("fonts: 2 removed (Lato, Lato Bold), 20 B -> 0 B") != std::string::npos);
    CHECK(text.find("layouts: 13 removed (layout x11, master, theme), 13 B -> 0 B") !=
          std::string::npos);
    CHECK(text.find("images: 6 downsampled (1x10 -> 1x1, 2x10 -> 1x1, 3x10 -> 1x1, 4x10 -> 1x1, "
                    "+2 more), 1 recompressed, 7 B -> 0 B") != std::string::npos);
}
