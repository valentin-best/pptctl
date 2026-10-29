// Golden and end-to-end tests for `pptctl optimize` (#105).
//
// Every fixture below has its `optimize --dry-run --json` report checked in
// under tests/golden/ as <fixture>.optimize.json. To regenerate after an
// intentional output change:
//   ./tests/golden/generate_golden.sh ./build/macos-release/pptctl
// Byte counts that depend on the zlib in use (system zlib on macOS and Linux,
// zlib-ng on Windows) are "<bytes>" in the goldens: the output size, and both
// sizes of every action that isn't a removal. They are checked here as
// invariants instead.
//
// Each fixture is then optimized for real, and the output must be:
//   * the same deck: `extract --format json` is identical except for picture
//     sizes, and `diff` pairs every slide, finding at most replaced pictures;
//   * stable: optimizing it again changes nothing.
// optimize_deck.pptx has work for every pass; with each pass's opt-out, the
// parts that pass would change come through byte-identical.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/optimize.hpp>
#include <pptctl/package.hpp>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef PPTCTL_GOLDEN_DIR
#define PPTCTL_GOLDEN_DIR "tests/golden"
#endif

namespace pptctl::cmd {
int optimize_to(const std::string& in,
                const OptimizeOptions& opts,
                const OptimizeOutputOptions& out,
                std::ostream& os);
int extract_to(const std::string& path, const std::string& format_str, std::ostream& os);
std::optional<DiffReport> diff_report(const std::string& a,
                                      const std::string& b,
                                      const DiffOptions& opts);
}  // namespace pptctl::cmd

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::array<std::string_view, 5> kFixtures = {
    "minimal", "notes_and_masters", "bloat_deck", "crop_deck", "optimize_deck"};

std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name) + ".pptx";
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_optimize_golden_" + run + "_" + std::to_string(counter++));
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

json read_golden(std::string_view name) {
    const std::string path = std::string(PPTCTL_GOLDEN_DIR) + "/" + std::string(name);
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.is_open(), "golden file missing: ", path);
    return json::parse(std::string(std::istreambuf_iterator<char>(f), {}));
}

// The `optimize --json` report for `in` (written to `out`, or a dry run).
json report_of(const std::string& in, const std::string& out = {}) {
    std::ostringstream os;
    pptctl::OptimizeOutputOptions output{.output = out, .dry_run = out.empty(), .json = true};
    REQUIRE(pptctl::cmd::optimize_to(in, {}, output, os) == 0);
    return json::parse(os.str());
}

// The golden rewrite (see generate_golden.sh), after checking what it blanks:
// the output is no larger than the input, a rewritten part shrinks and a
// skipped one keeps its size.
json normalized(json report) {
    CHECK(report["output_bytes"].get<std::uint64_t>() <=
          report["input_bytes"].get<std::uint64_t>());
    report["input"] = "<path>";
    report["output_bytes"] = "<bytes>";
    for (auto& pass : report["passes"]) {
        for (auto& a : pass["actions"]) {
            if (a["kind"] == "removed") {
                continue;
            }
            const auto before = a["bytes_before"].get<std::uint64_t>();
            const auto after = a["bytes_after"].get<std::uint64_t>();
            if (a["kind"] == "skipped") {
                CHECK(after == before);
            } else {
                CHECK_MESSAGE(after < before, a["part"].get<std::string>());
            }
            a["bytes_before"] = "<bytes>";
            a["bytes_after"] = "<bytes>";
        }
    }
    return report;
}

// `extract --format json`, without the input path and the picture sizes:
// optimize may re-encode an image, but nothing else on a slide may change.
json extracted(const std::string& path) {
    std::ostringstream os;
    REQUIRE(pptctl::cmd::extract_to(path, "json", os) == 0);
    auto doc = json::parse(os.str());
    doc.erase("file");
    const auto strip = [](auto& self, json& shapes) -> void {
        for (auto& shape : shapes) {
            if (shape["picture"].is_object()) {
                shape["picture"].erase("size_bytes");
            }
            self(self, shape["children"]);
        }
    };
    for (auto& slide : doc["slides"]) {
        strip(strip, slide["shapes"]);
    }
    return doc;
}

// The parts of `pkg` whose name starts with any of `prefixes`, by name.
std::vector<std::string> parts_under(const pptctl::Package& pkg,
                                     std::initializer_list<std::string_view> prefixes) {
    std::vector<std::string> out;
    for (const auto& name : pkg.part_names()) {
        for (const auto prefix : prefixes) {
            if (name.starts_with(prefix)) {
                out.push_back(name);
                break;
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("optimize golden: each fixture's report matches its golden") {
    for (const auto name : kFixtures) {
        CAPTURE(name);
        const auto actual = normalized(report_of(fixture(name)));
        const auto golden = read_golden(std::string(name) + ".optimize.json");
        CHECK_MESSAGE(actual == golden, actual.dump(2));
    }
}

TEST_CASE("optimize golden: the output is the same deck") {
    for (const auto name : kFixtures) {
        CAPTURE(name);
        ScratchDir dir;
        const auto in = fixture(name);
        const auto out = (dir.path / "out.pptx").string();
        (void)report_of(in, out);

        CHECK(extracted(in) == extracted(out));

        const auto report = pptctl::cmd::diff_report(in, out, {});
        REQUIRE(report.has_value());
        for (const auto& slide : report->slides) {
            CAPTURE(slide.a_index);
            CHECK(slide.status != pptctl::SlideStatus::Added);
            CHECK(slide.status != pptctl::SlideStatus::Removed);
            CHECK_FALSE(slide.moved);
            const auto& c = slide.changes;
            CHECK(c.text.empty());
            CHECK(c.shapes.empty());
            CHECK(c.tables.empty());
            CHECK(c.charts.empty());
            for (const auto& pic : c.pictures) {
                CHECK(pic.op == pptctl::PictureOp::Replaced);
            }
        }
    }
}

TEST_CASE("optimize golden: optimizing the output again changes nothing") {
    for (const auto name : kFixtures) {
        CAPTURE(name);
        ScratchDir dir;
        const auto once = (dir.path / "once.pptx").string();
        const auto twice = (dir.path / "twice.pptx").string();
        (void)report_of(fixture(name), once);
        const auto again = report_of(once, twice);
        CHECK(again["output_bytes"] == again["input_bytes"]);
        for (const auto& pass : again["passes"]) {
            for (const auto& a : pass["actions"]) {
                CHECK_MESSAGE(a["kind"] == "skipped", a.dump());
            }
        }
    }
}

TEST_CASE("optimize golden: each opt-out leaves its pass's parts byte-identical") {
    pptctl::Package in;
    REQUIRE(in.open(fixture("optimize_deck")));

    struct OptOut {
        const char* flag;
        bool pptctl::OptimizeOptions::*pass;
        std::vector<std::string> parts;  // must come through unchanged
    };
    const std::vector<OptOut> cases = {
        {"--no-layouts",
         &pptctl::OptimizeOptions::layouts,
         parts_under(in, {"ppt/slideLayouts/", "ppt/slideMasters/", "ppt/theme/"})},
        {"--keep-fonts", &pptctl::OptimizeOptions::fonts, {"ppt/fonts/font1.fntdata"}},
        // The crop pass's edits are the srcRect in the slide and the media;
        // the images pass may still recompress that media, so check the slide.
        {"--no-crop", &pptctl::OptimizeOptions::crop, {"ppt/slides/slide3.xml"}},
        {"--no-images",
         &pptctl::OptimizeOptions::images,
         {"ppt/media/image1.jpg", "ppt/media/image2.png"}},
        {"--no-orphans", &pptctl::OptimizeOptions::orphans, {"ppt/media/orphan.png"}},
    };
    for (const auto& c : cases) {
        CAPTURE(c.flag);
        REQUIRE_FALSE(c.parts.empty());
        ScratchDir dir;
        const auto out_path = (dir.path / "out.pptx").string();
        pptctl::OptimizeOptions opts;
        opts.*c.pass = false;
        std::string error;
        REQUIRE_MESSAGE(pptctl::optimize(fixture("optimize_deck"), out_path, opts, error), error);
        pptctl::Package out;
        REQUIRE(out.open(out_path));
        for (const auto& part : c.parts) {
            CAPTURE(part);
            const auto size = out.part_size(part);
            REQUIRE(size.has_value());
            CHECK(out.read_part(part) == in.read_part(part));
        }
    }

    // With every pass on, each of those parts does change (or go), so the
    // checks above are not vacuous.
    ScratchDir dir;
    const auto out_path = (dir.path / "out.pptx").string();
    std::string error;
    REQUIRE_MESSAGE(pptctl::optimize(fixture("optimize_deck"), out_path, {}, error), error);
    pptctl::Package out;
    REQUIRE(out.open(out_path));
    for (const char* part : {"ppt/slideLayouts/slideLayout1.xml",
                             "ppt/fonts/font1.fntdata",
                             "ppt/slides/slide3.xml",
                             "ppt/media/image1.jpg",
                             "ppt/media/image2.png",
                             "ppt/media/orphan.png"}) {
        CAPTURE(part);
        CHECK(out.read_part(part) != in.read_part(part));
    }
}
