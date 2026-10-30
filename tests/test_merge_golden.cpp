// Golden and end-to-end tests for `pptctl merge` (#140).
//
// Each case below has its `merge --json` report and the output's part listing
// (uncompressed size and name, in ZIP order) checked in under
// tests/golden/merge/ as <case>.merge.json and <case>.parts.txt. To regenerate
// after an intentional output change:
//   ./tests/golden/generate_golden.sh ./build/macos-release/pptctl
// The output path and size (which depends on the zlib in use) are
// placeholders in the report golden.
//
// Every output, golden case or edge input, must also be:
//   * well-formed: every integrity rule passes, through the library;
//   * the same slides: the diff engine pairs each input slide with its place
//     in the output, unchanged (text, shapes, pictures by hash, tables, charts);
//   * surgical: the base deck's parts come through byte for byte, bar the
//     package-level parts a merge rewrites;
//   * deterministic: the same inputs give byte-identical output.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/presentation.hpp>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef PPTCTL_GOLDEN_DIR
#define PPTCTL_GOLDEN_DIR "tests/golden"
#endif

namespace pptctl::cmd {
int merge_to(const std::vector<std::string>& inputs,
             const MergeOptions& opts,
             const MergeOutputOptions& out,
             std::ostream& os);
std::optional<DiffReport> diff_report(const std::string& a,
                                      const std::string& b,
                                      const DiffOptions& opts);
}  // namespace pptctl::cmd

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name) + ".pptx";
}

std::string read_golden(const std::string& name) {
    const auto path = std::string(PPTCTL_GOLDEN_DIR) + "/merge/" + name;
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.is_open(), "golden file missing: ", path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_golden_" + run + "_" + std::to_string(counter++));
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

struct Case {
    const char* name;
    std::vector<std::string_view> decks;
    std::size_t theme = 0;
};

const std::vector<Case>& golden_cases() {
    static const std::vector<Case> cases{
        {"a_b", {"merge_a", "merge_b"}},
        {"a_c", {"merge_a", "merge_c"}},
        {"a_b_c", {"merge_a", "merge_b", "merge_c"}},
        {"a_b_c_theme2", {"merge_a", "merge_b", "merge_c"}, 2},
    };
    return cases;
}

std::vector<std::string> paths_of(const std::vector<std::string_view>& decks) {
    std::vector<std::string> out;
    for (const auto d : decks) {
        out.push_back(fixture(d));
    }
    return out;
}

// Merge `decks` into `out` through the command; returns its JSON report.
json merge_json(const std::vector<std::string_view>& decks,
                std::size_t theme,
                const std::string& out) {
    std::ostringstream os;
    REQUIRE(pptctl::cmd::merge_to(
                paths_of(decks), {.theme = theme}, {.output = out, .json = true}, os) == 0);
    return json::parse(os.str());
}

// The report with machine-specific fields replaced: input paths by their file
// name, the output path and size by placeholders.
json normalized(json report) {
    for (auto& input : report["inputs"]) {
        input["path"] = fs::path(input["path"].get<std::string>()).filename().string();
    }
    report["output"] = "<path>";
    report["output_bytes"] = "<bytes>";
    return report;
}

// "size name" per part, in ZIP order: the shape of `unzip -l`.
std::string part_listing(const pptctl::Package& pkg) {
    std::string out;
    for (const auto& name : pkg.part_names()) {
        out += std::to_string(pkg.part_size(name).value_or(0)) + " " + name + "\n";
    }
    return out;
}

std::string messages(const std::vector<pptctl::Diagnostic>& ds) {
    std::string out;
    for (const auto& d : ds) {
        out += d.rule_id + ": " + d.message + "\n";
    }
    return out;
}

std::size_t slide_count(const std::string& path) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(path));
    pptctl::Presentation pres(pkg);
    REQUIRE(pres.load());
    return pres.slide_count();
}

// The properties every merge output must have, given its inputs in order.
void check_output(const std::vector<std::string>& inputs,
                  std::size_t theme,
                  const std::string& out) {
    pptctl::Package merged;
    REQUIRE(merged.open(out));

    // Well-formed.
    const auto ds = pptctl::check_integrity(merged, pptctl::PartGraph::build(merged));
    CHECK_MESSAGE(ds.empty(), messages(ds));

    // The same slides: input k's slide i is output slide offset_k + i,
    // unchanged. A deck given twice has identical copies, which the diff
    // engine may pair either way, so any of its offsets will do.
    std::vector<std::size_t> offsets;
    std::size_t offset = 0;
    for (const auto& input : inputs) {
        offsets.push_back(offset);
        offset += slide_count(input);
    }
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        CAPTURE(inputs[k]);
        const auto report = pptctl::cmd::diff_report(inputs[k], out, {});
        REQUIRE(report);
        const auto n = slide_count(inputs[k]);
        for (std::size_t i = 1; i <= n; ++i) {
            CAPTURE(i);
            const auto it = std::find_if(
                report->slides.begin(), report->slides.end(), [&](const pptctl::SlideDiff& s) {
                    return s.a_index == i;
                });
            REQUIRE(it != report->slides.end());
            CHECK(it->status == pptctl::SlideStatus::Unchanged);
            bool placed = false;
            for (std::size_t j = 0; j < inputs.size(); ++j) {
                placed = placed || (inputs[j] == inputs[k] && it->b_index == offsets[j] + i);
            }
            CHECK_MESSAGE(placed, "paired with output slide ", it->b_index);
        }
    }
    CHECK(slide_count(out) == offset);

    // Surgical: the base's parts come through byte for byte, except those a
    // merge rewrites.
    const std::set<std::string> rewritten{"[Content_Types].xml",
                                          "ppt/presentation.xml",
                                          "ppt/_rels/presentation.xml.rels",
                                          "docProps/app.xml"};
    pptctl::Package base;
    REQUIRE(base.open(inputs[theme == 0 ? 0 : theme - 1]));
    for (const auto& name : base.part_names()) {
        if (!rewritten.contains(name)) {
            CHECK_MESSAGE(merged.read_part(name) == base.read_part(name), name);
        }
    }
}

}  // namespace

// ── goldens ───────────────────────────────────────────────────────────────────

TEST_CASE("merge golden: each case's report and part listing match their goldens") {
    for (const auto& c : golden_cases()) {
        CAPTURE(c.name);
        ScratchDir dir;
        const auto out = (dir.path / "out.pptx").string();
        const auto report = normalized(merge_json(c.decks, c.theme, out));
        const auto golden = json::parse(read_golden(std::string(c.name) + ".merge.json"));
        CHECK_MESSAGE(report == golden, report.dump(2));

        pptctl::Package pkg;
        REQUIRE(pkg.open(out));
        CHECK(part_listing(pkg) == read_golden(std::string(c.name) + ".parts.txt"));
    }
}

TEST_CASE("merge golden: each case's output is well-formed, the same slides, and surgical") {
    for (const auto& c : golden_cases()) {
        CAPTURE(c.name);
        ScratchDir dir;
        const auto out = (dir.path / "out.pptx").string();
        (void)merge_json(c.decks, c.theme, out);
        check_output(paths_of(c.decks), c.theme, out);
    }
}

TEST_CASE("merge golden: the same inputs give byte-identical output") {
    ScratchDir dir;
    const auto first = (dir.path / "first.pptx").string();
    const auto second = (dir.path / "second.pptx").string();
    const std::vector<std::string_view> decks{"merge_a", "merge_b", "merge_c", "merge_edge"};
    (void)merge_json(decks, 0, first);
    // Past the 2-second resolution of ZIP dates, so a wall-clock date would show.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    (void)merge_json(decks, 0, second);
    std::ifstream a(first, std::ios::binary);
    std::ifstream b(second, std::ios::binary);
    const std::string bytes_a{std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>()};
    const std::string bytes_b{std::istreambuf_iterator<char>(b), std::istreambuf_iterator<char>()};
    CHECK(bytes_a.size() == bytes_b.size());
    CHECK(bytes_a == bytes_b);
}

// ── edge inputs ───────────────────────────────────────────────────────────────

TEST_CASE("merge edge: a deck with no slides, on either side") {
    for (const auto& decks : {std::vector<std::string_view>{"merge_a", "merge_empty"},
                              std::vector<std::string_view>{"merge_empty", "merge_a"}}) {
        CAPTURE(decks.front());
        ScratchDir dir;
        const auto out = (dir.path / "out.pptx").string();
        const auto report = merge_json(decks, 0, out);
        CHECK(report["slides"] == 3);
        check_output(paths_of(decks), 0, out);
    }
}

TEST_CASE(
    "merge edge: notes, a chart's workbook, links, a linked video and SmartArt arrive whole") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    const std::vector<std::string_view> decks{"merge_a", "merge_edge"};
    (void)merge_json(decks, 0, out);
    check_output(paths_of(decks), 0, out);

    pptctl::Package merged;
    REQUIRE(merged.open(out));
    // merge_edge's slides are output slides 4-6.
    const auto rels_of = [&](const char* slide) { return merged.relationships(slide); };
    const auto has_rel = [](const std::vector<pptctl::Relationship>& rels,
                            std::string_view type_suffix,
                            std::string_view target,
                            bool external) {
        return std::any_of(rels.begin(), rels.end(), [&](const pptctl::Relationship& r) {
            return r.type.ends_with(type_suffix) && r.target == target && r.external == external;
        });
    };

    // Slide 4: notes, and a chart that keeps its embedded workbook.
    const auto chart_slide = rels_of("ppt/slides/slide4.xml");
    CHECK(has_rel(chart_slide, "/notesSlide", "ppt/notesSlides/notesSlide1.xml", false));
    CHECK(has_rel(chart_slide, "/chart", "ppt/charts/chart1.xml", false));
    CHECK(has_rel(merged.relationships("ppt/charts/chart1.xml"),
                  "/package",
                  "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx",
                  false));

    // Slide 5: the hyperlink and the linked video stay external; the poster is copied.
    const auto link_slide = rels_of("ppt/slides/slide5.xml");
    CHECK(has_rel(link_slide, "/hyperlink", "https://example.com/edge", true));
    CHECK(has_rel(link_slide, "/video", "file:///Users/Shared/pptctl-clip.mp4", true));
    CHECK(has_rel(link_slide, "/media", "file:///Users/Shared/pptctl-clip.mp4", true));

    // Slide 6: all five SmartArt parts, each declared.
    const auto diagram_slide = rels_of("ppt/slides/slide6.xml");
    for (const auto* part : {"data1", "layout1", "quickStyle1", "colors1", "drawing1"}) {
        const auto name = std::string("ppt/diagrams/") + part + ".xml";
        CHECK_MESSAGE(merged.part_size(name), name);
        CHECK_MESSAGE(!merged.content_type(name).empty(), name);
        CHECK(std::any_of(diagram_slide.begin(), diagram_slide.end(), [&](const auto& r) {
            return r.target == name;
        }));
    }
}

TEST_CASE("merge edge: a deck merged with itself shares media, never single-owner parts") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    const std::vector<std::string_view> decks{"merge_edge", "merge_edge"};
    const auto report = merge_json(decks, 0, out);
    CHECK(report["inputs"][1]["media_deduplicated"]["count"] == 1);  // the video poster
    CHECK(report["inputs"][1]["masters"]["reused"] == 1);
    check_output(paths_of(decks), 0, out);

    pptctl::Package merged;
    REQUIRE(merged.open(out));
    // Charts, workbooks, notes and diagrams are copied, one per slide.
    for (const auto* part : {"ppt/charts/chart2.xml",
                             "ppt/embeddings/Microsoft_Excel_Sheet2.xlsx",
                             "ppt/notesSlides/notesSlide2.xml",
                             "ppt/diagrams/data2.xml",
                             "ppt/diagrams/drawing2.xml"}) {
        CHECK_MESSAGE(merged.part_size(part), part);
    }
}
