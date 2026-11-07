// Tests for merge's report of charts Google Slides will draw in another theme
// (#167).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
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

constexpr std::string_view kWarning =
    "1 chart on slides with another theme than the first deck's: Google Slides draws it in the "
    "first deck's theme colours and fonts";

std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name);
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_charts_" + run + "_" + std::to_string(counter++));
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

// The chart warnings of each input of a dry-run merge of `inputs`.
std::vector<std::vector<std::string>> chart_warnings(const std::vector<std::string>& inputs,
                                                     const pptctl::MergeOptions& opts = {}) {
    std::string error;
    const auto report = pptctl::merge(inputs, "", opts, error);
    REQUIRE_MESSAGE(report, error);
    std::vector<std::vector<std::string>> out;
    for (const auto& input : report->inputs) {
        auto& w = out.emplace_back();
        std::copy_if(input.warnings.begin(),
                     input.warnings.end(),
                     std::back_inserter(w),
                     [](const std::string& s) { return s.find("chart") != std::string::npos; });
    }
    return out;
}

using Warnings = std::vector<std::vector<std::string>>;

// merge_a's theme with `from` replaced by `to` (once), saved in `dir`, and
// theme_look() of the result.
std::string look_of_edited_theme(const ScratchDir& dir,
                                 std::string_view from,
                                 std::string_view to) {
    const std::string theme = "ppt/theme/theme1.xml";
    pptctl::Package src;
    REQUIRE(src.open(fixture("merge_a.pptx")));
    const auto bytes = src.read_part(theme);
    std::string xml(bytes.size(), '\0');
    std::transform(
        bytes.begin(), bytes.end(), xml.begin(), [](std::byte b) { return static_cast<char>(b); });
    const auto at = xml.find(from);
    REQUIRE(at != std::string::npos);
    xml.replace(at, from.size(), to);
    std::vector<std::byte> edited(xml.size());
    std::transform(
        xml.begin(), xml.end(), edited.begin(), [](char c) { return static_cast<std::byte>(c); });
    pptctl::PartChanges changes;
    changes.replaced[theme] = std::move(edited);
    const auto path = (dir.path / ("theme_" + std::to_string(ScratchDir::counter++) + ".pptx"));
    REQUIRE(src.write(path.string(), changes));
    pptctl::Package pkg;
    REQUIRE(pkg.open(path.string()));
    return pptctl::detail::theme_look(pkg, theme);
}

}  // namespace

TEST_CASE("merge charts: a chart imported on another theme is reported") {
    // merge_b has its own theme (red accents) and a chart on slide 2.
    CHECK(chart_warnings({fixture("merge_a.pptx"), fixture("merge_b.pptx")}) ==
          Warnings{{}, {std::string(kWarning)}});
}

TEST_CASE("merge charts: a chart on the base deck's own master is not") {
    // merge_edge's chart slide uses a master identical to merge_a's.
    CHECK(chart_warnings({fixture("merge_a.pptx"), fixture("merge_edge.pptx")}) ==
          Warnings{{}, {}});
}

TEST_CASE("merge charts: the base deck's own charts are never reported") {
    CHECK(chart_warnings({fixture("merge_b.pptx"), fixture("merge_a.pptx")}) == Warnings{{}, {}});
    // With merge_edge as the base, merge_b's chart is on another theme again.
    CHECK(chart_warnings({fixture("merge_edge.pptx"), fixture("merge_b.pptx")}) ==
          Warnings{{}, {std::string(kWarning)}});
}

TEST_CASE("merge charts: --theme puts every chart on the base's theme, silently") {
    pptctl::MergeOptions opts;
    opts.theme = 1;
    CHECK(chart_warnings({fixture("merge_a.pptx"), fixture("merge_b.pptx")}, opts) ==
          Warnings{{}, {}});
}

TEST_CASE("theme_look: colours and major/minor fonts count, the theme's name doesn't") {
    pptctl::Package a;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    const auto base = pptctl::detail::theme_look(a, "ppt/theme/theme1.xml");
    REQUIRE_FALSE(base.empty());
    CHECK(pptctl::detail::first_theme_look(a) == base);

    const ScratchDir dir;
    CHECK(look_of_edited_theme(dir, R"(name="Office Theme")", R"(name="Renamed")") == base);
    CHECK(look_of_edited_theme(dir,
                               R"(<a:accent1><a:srgbClr val=")",
                               R"(<a:accent1><a:srgbClr val="C0FFEE"/><a:x val=")") != base);
    CHECK(look_of_edited_theme(dir,
                               R"(<a:minorFont><a:latin typeface=")",
                               R"(<a:minorFont><a:latin typeface="Arial"/><a:x typeface=")") !=
          base);
    CHECK(pptctl::detail::theme_look(a, "").empty());
    CHECK(pptctl::detail::theme_look(a, "ppt/theme/missing.xml").empty());
}
