// Tests for the merge pipeline and the merge command.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace pptctl::cmd {
int merge_to(const std::vector<std::string>& inputs,
             const MergeOptions& opts,
             const MergeOutputOptions& out,
             std::ostream& os);
void render_merge(const MergeReport& report,
                  const MergeOptions& opts,
                  const MergeOutputOptions& out,
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
               fs::path("pptctl_merge_" + run + "_" + std::to_string(counter++));
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

const std::vector<std::string>& abc() {
    static const std::vector<std::string> inputs{
        fixture("merge_a.pptx"), fixture("merge_b.pptx"), fixture("merge_c.pptx")};
    return inputs;
}

// True if every part of the package at `base` is in the one at `out` with the
// same bytes, except `[Content_Types].xml`, and `out` has more parts.
bool keeps_base(const std::string& base, const std::string& out) {
    pptctl::Package pb;
    pptctl::Package po;
    REQUIRE(pb.open(base));
    REQUIRE(po.open(out));
    for (const auto& name : pb.part_names()) {
        if (name != "[Content_Types].xml" && pb.read_part(name) != po.read_part(name)) {
            return false;
        }
    }
    return po.part_count() > pb.part_count();
}

// Integrity diagnostics on the package at `path`, except orphan-part: the
// imported slides aren't in the slide list yet (#136), so nothing reaches them.
std::vector<pptctl::Diagnostic> integrity_problems(const std::string& path) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(path));
    auto ds = pptctl::check_integrity(pkg, pptctl::PartGraph::build(pkg));
    std::erase_if(ds, [](const pptctl::Diagnostic& d) { return d.rule_id == "orphan-part"; });
    return ds;
}

}  // namespace

// ── fixtures ──────────────────────────────────────────────────────────────────

TEST_CASE("merge fixtures: a and c share the logo byte for byte; b has its own theme") {
    pptctl::Package a;
    pptctl::Package b;
    pptctl::Package c;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    REQUIRE(b.open(fixture("merge_b.pptx")));
    REQUIRE(c.open(fixture("merge_c.pptx")));
    CHECK(a.part_hash("ppt/media/image1.png") == c.part_hash("ppt/media/image1.png"));
    CHECK(a.read_part("ppt/theme/theme1.xml") == c.read_part("ppt/theme/theme1.xml"));
    CHECK(a.read_part("ppt/theme/theme1.xml") != b.read_part("ppt/theme/theme1.xml"));
    CHECK(b.part_size("ppt/notesSlides/notesSlide2.xml"));
    CHECK(b.part_size("ppt/charts/chart1.xml"));
}

// ── pipeline ──────────────────────────────────────────────────────────────────

TEST_CASE("merge: deck 1 is the base; the other decks' slides arrive as added parts") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::string error;
    const auto report = pptctl::merge(abc(), out, {}, error);
    REQUIRE(report);
    CHECK(error.empty());
    CHECK(keeps_base(fixture("merge_a.pptx"), out));
    CHECK(integrity_problems(out).empty());
    CHECK(report->output_bytes == fs::file_size(out));
    CHECK(report->slides == 3);

    REQUIRE(report->inputs.size() == 3);
    const auto& a = report->inputs[0];
    CHECK(a.path == fixture("merge_a.pptx"));
    CHECK(a.base);
    CHECK(a.bytes == fs::file_size(fixture("merge_a.pptx")));
    CHECK(a.slides == 3);
    CHECK(a.warnings.empty());
    for (const auto& input : {report->inputs[1], report->inputs[2]}) {
        CHECK_FALSE(input.base);
        CHECK(input.slides == 0);
        CHECK(input.parts_added > 0);
        CHECK(input.masters_imported == 1);
        CHECK(input.warnings.size() == 1);
    }
}

TEST_CASE("merge: --theme=N makes deck N the base package") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::string error;
    const auto report = pptctl::merge(abc(), out, {.theme = 2}, error);
    REQUIRE(report);
    CHECK(keeps_base(fixture("merge_b.pptx"), out));
    CHECK_FALSE(report->inputs[0].base);
    CHECK(report->inputs[1].base);
    CHECK(report->inputs[1].slides == 2);
    CHECK(report->slides == 2);
}

TEST_CASE("merge: a dry run measures the output and writes nothing") {
    ScratchDir dir;
    std::string error;
    const auto report = pptctl::merge(abc(), "", {}, error);
    REQUIRE(report);
    CHECK(report->output_bytes > 0);
    CHECK(fs::is_empty(dir.path));
}

TEST_CASE("merge: errors") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::string error;

    SUBCASE("fewer than two inputs") {
        const std::vector<std::string> one{fixture("merge_a.pptx")};
        CHECK_FALSE(pptctl::merge(one, out, {}, error));
        CHECK(error.find("at least two") != std::string::npos);
    }
    SUBCASE("--theme past the last input") {
        CHECK_FALSE(pptctl::merge(abc(), out, {.theme = 4}, error));
        CHECK(error.find("--theme=4") != std::string::npos);
    }
    SUBCASE("an input that isn't a package") {
        const auto bogus = (dir.path / "bogus.pptx").string();
        std::ofstream(bogus) << "not a zip";
        const std::vector<std::string> inputs{fixture("merge_a.pptx"), bogus};
        CHECK_FALSE(pptctl::merge(inputs, out, {}, error));
        CHECK(error.find("bogus.pptx") != std::string::npos);
    }
    SUBCASE("an output directory that doesn't exist") {
        const auto nowhere = (dir.path / "missing" / "out.pptx").string();
        CHECK_FALSE(pptctl::merge(abc(), nowhere, {}, error));
        CHECK(error.find("cannot write") != std::string::npos);
    }
    CHECK_FALSE(fs::exists(out));
}

// ── command ───────────────────────────────────────────────────────────────────

TEST_CASE("merge command: argument checks exit 1 and write nothing") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::ostringstream os;

    SUBCASE("no -o and no --dry-run") {
        CHECK(pptctl::cmd::merge_to(abc(), {}, {}, os) == 1);
    }
    SUBCASE("-o names an input") {
        const auto copy = (dir.path / "a.pptx").string();
        fs::copy_file(fixture("merge_a.pptx"), copy);
        const std::vector<std::string> inputs{fixture("merge_b.pptx"), copy};
        CHECK(pptctl::cmd::merge_to(inputs, {}, {.output = copy}, os) == 1);
        CHECK(fs::file_size(copy) == fs::file_size(fixture("merge_a.pptx")));
    }
    SUBCASE("--theme out of range") {
        CHECK(pptctl::cmd::merge_to(abc(), {.theme = 9}, {.output = out}, os) == 1);
    }
    SUBCASE("one input") {
        const std::vector<std::string> one{fixture("merge_a.pptx")};
        CHECK(pptctl::cmd::merge_to(one, {}, {.output = out}, os) == 1);
    }
    CHECK(os.str().empty());
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("merge command: writes -o and prints the summary") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::ostringstream os;
    CHECK(pptctl::cmd::merge_to(abc(), {}, {.output = out}, os) == 0);
    CHECK(fs::exists(out));
    const auto text = os.str();
    CHECK(text.starts_with("merge -> " + out + "\n"));
    CHECK(text.find("  1. " + fixture("merge_a.pptx") + ": 3 slides (base)\n") !=
          std::string::npos);
    CHECK(text.find("  2. " + fixture("merge_b.pptx") + ": 0 slides, ") != std::string::npos);
    CHECK(text.find("warning: 2 slides imported but not added to the slide list yet") !=
          std::string::npos);
    CHECK(text.find("total: 3 slides, ") != std::string::npos);
}

TEST_CASE("merge command: --dry-run --json reports without writing") {
    ScratchDir dir;
    const auto out = (dir.path / "out.pptx").string();
    std::ostringstream os;
    CHECK(pptctl::cmd::merge_to(
              abc(), {.theme = 3}, {.output = out, .dry_run = true, .json = true}, os) == 0);
    CHECK_FALSE(fs::exists(out));
    const auto j = nlohmann::json::parse(os.str());
    CHECK(j["schema_version"] == 1);
    CHECK(j["output"].is_null());
    CHECK(j["dry_run"] == true);
    CHECK(j["theme"] == 3);
    CHECK(j["slides"] == 2);
    REQUIRE(j["inputs"].size() == 3);
    CHECK(j["inputs"][2]["base"] == true);
    CHECK(j["inputs"][2]["path"] == fixture("merge_c.pptx"));
}

// ── renderers ─────────────────────────────────────────────────────────────────

namespace {

pptctl::MergeReport sample_report() {
    pptctl::MergeReport r;
    pptctl::MergeInputReport a;
    a.path = "a.pptx";
    a.base = true;
    a.bytes = 1000;
    a.slides = 3;
    pptctl::MergeInputReport b;
    b.path = "b.pptx";
    b.bytes = 2000;
    b.slides = 1;
    b.parts_added = 6;
    b.masters_imported = 1;
    pptctl::MergeInputReport c;
    c.path = "c.pptx";
    c.bytes = 3000;
    c.slides = 2;
    c.parts_added = 4;
    c.media_deduplicated = 2;
    c.media_bytes_saved = 4096;
    c.masters_reused = 1;
    c.warnings = {"chart data not carried over"};
    r.inputs = {a, b, c};
    r.slides = 6;
    r.output_bytes = 5120;
    return r;
}

}  // namespace

TEST_CASE("render_merge: human summary lists each input, then the total") {
    std::ostringstream os;
    pptctl::cmd::render_merge(sample_report(), {}, {.output = "out.pptx"}, os);
    CHECK(os.str() ==
          "merge -> out.pptx\n"
          "  1. a.pptx: 3 slides (base)\n"
          "  2. b.pptx: 1 slide, 6 parts added, 1 master imported\n"
          "  3. c.pptx: 2 slides, 4 parts added, 2 media deduplicated (4.0 KiB), 1 master "
          "reused\n"
          "     warning: chart data not carried over\n"
          "total: 6 slides, 5.0 KiB\n");
}

TEST_CASE("render_merge: JSON carries every report field") {
    std::ostringstream os;
    pptctl::cmd::render_merge(sample_report(), {}, {.output = "out.pptx", .json = true}, os);
    const auto j = nlohmann::json::parse(os.str());
    CHECK(j["output"] == "out.pptx");
    CHECK(j["dry_run"] == false);
    CHECK(j["theme"].is_null());
    CHECK(j["slides"] == 6);
    CHECK(j["output_bytes"] == 5120);
    const auto& c = j["inputs"][2];
    CHECK(c["path"] == "c.pptx");
    CHECK(c["base"] == false);
    CHECK(c["bytes"] == 3000);
    CHECK(c["slides"] == 2);
    CHECK(c["parts_added"] == 4);
    CHECK(c["media_deduplicated"]["count"] == 2);
    CHECK(c["media_deduplicated"]["bytes"] == 4096);
    CHECK(c["masters"]["imported"] == 0);
    CHECK(c["masters"]["reused"] == 1);
    CHECK(c["warnings"] == nlohmann::json::array({"chart data not carried over"}));
}
