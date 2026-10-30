// Tests for --theme=N: restyling foreign slides onto one deck's layouts (#138).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <pptctl/integrity.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <pptctl/presentation.hpp>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../src/lib/merge_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;

using pptctl::detail::LayoutMatcher;
using pptctl::detail::LayoutMatchKind;

constexpr std::string_view kSlideLayoutRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout";

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

// A fresh scratch directory per test, removed on scope exit.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_merge_theme_" + run + "_" + std::to_string(counter++));
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
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::vector<std::byte> to_bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

// merge_c with its "Title Only" layout (used by slide 2) turned into a custom
// layout: sldLayout@type "titleOnly" -> "cust", and renamed to `name`.
std::string with_custom_layout(const fs::path& dir, const std::string& name) {
    constexpr const char* kLayout = "ppt/slideLayouts/slideLayout6.xml";
    pptctl::Package c;
    REQUIRE(c.open(fixture("merge_c.pptx")));
    auto xml = to_string(c.read_part(kLayout));
    const auto type = xml.find(R"(type="titleOnly")");
    REQUIRE(type != std::string::npos);
    xml.replace(type, std::string_view(R"(type="titleOnly")").size(), R"(type="cust")");
    const std::string old_name = R"(<p:cSld name="Title Only">)";
    const auto at = xml.find(old_name);
    REQUIRE(at != std::string::npos);
    xml.replace(at, old_name.size(), "<p:cSld name=\"" + name + "\">");
    pptctl::PartChanges changes;
    changes.replaced[kLayout] = to_bytes(xml);
    const auto out = (dir / ("custom_" + std::to_string(name.size()) + ".pptx")).string();
    REQUIRE(c.write(out, changes));
    return out;
}

struct Merged {
    ScratchDir dir;
    pptctl::MergeReport report;
    pptctl::Package pkg;

    Merged(const std::vector<std::string>& inputs, std::size_t theme) {
        const auto out = (dir.path / "out.pptx").string();
        std::string error;
        auto r = pptctl::merge(inputs, out, {.theme = theme}, error);
        REQUIRE_MESSAGE(r, error);
        report = std::move(*r);
        REQUIRE(pkg.open(out));
    }

    [[nodiscard]] std::size_t count(std::string_view dir_prefix) const {
        const auto names = pkg.part_names();
        return static_cast<std::size_t>(
            std::count_if(names.begin(), names.end(), [&](const std::string& n) {
                return n.starts_with(dir_prefix) && n.ends_with(".xml");
            }));
    }

    // Each slide's layout, in slide-list order.
    [[nodiscard]] std::vector<std::string> slide_layouts() const {
        pptctl::Presentation pres(pkg);
        REQUIRE(pres.load());
        std::vector<std::string> out;
        for (const auto& slide : pres.slide_parts()) {
            for (const auto& rel : pkg.relationships(slide)) {
                if (rel.type == kSlideLayoutRel) {
                    out.push_back(rel.target);
                }
            }
        }
        return out;
    }

    void check_integrity() const {
        const auto ds = pptctl::check_integrity(pkg, pptctl::PartGraph::build(pkg));
        std::string messages;
        for (const auto& d : ds) {
            messages += d.rule_id + ": " + d.message + "\n";
        }
        CHECK_MESSAGE(ds.empty(), messages);
    }
};

}  // namespace

// ── LayoutMatcher ─────────────────────────────────────────────────────────────

TEST_CASE("LayoutMatcher: same-type layouts map onto each other") {
    pptctl::Package a;
    pptctl::Package b;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    REQUIRE(b.open(fixture("merge_b.pptx")));
    const LayoutMatcher matcher(a);
    for (int i = 1; i <= 11; ++i) {
        const auto layout = "ppt/slideLayouts/slideLayout" + std::to_string(i) + ".xml";
        const auto m = matcher.match(b, layout);
        REQUIRE(m);
        CHECK(m->kind == LayoutMatchKind::type);
        CHECK(m->part == layout);  // same template order
        CHECK(m->source_name == m->target_name);
    }
}

TEST_CASE("LayoutMatcher: a custom layout matches by name, else falls back to Title and Content") {
    ScratchDir dir;
    pptctl::Package a;
    REQUIRE(a.open(fixture("merge_a.pptx")));
    const LayoutMatcher matcher(a);

    pptctl::Package same_name;
    REQUIRE(same_name.open(with_custom_layout(dir.path, "Title Only")));
    const auto by_name = matcher.match(same_name, "ppt/slideLayouts/slideLayout6.xml");
    REQUIRE(by_name);
    CHECK(by_name->kind == LayoutMatchKind::name);
    CHECK(by_name->part == "ppt/slideLayouts/slideLayout6.xml");

    pptctl::Package unknown;
    REQUIRE(unknown.open(with_custom_layout(dir.path, "Quarterly Numbers")));
    const auto fallback = matcher.match(unknown, "ppt/slideLayouts/slideLayout6.xml");
    REQUIRE(fallback);
    CHECK(fallback->kind == LayoutMatchKind::fallback);
    CHECK(fallback->part == "ppt/slideLayouts/slideLayout2.xml");
    CHECK(fallback->source_name == "Quarterly Numbers");
    CHECK(fallback->target_name == "Title and Content");
}

// ── merge ─────────────────────────────────────────────────────────────────────

TEST_CASE("merge: by default each deck keeps its own theme") {
    const Merged m({fixture("merge_a.pptx"), fixture("merge_b.pptx")}, 0);
    CHECK(m.count("ppt/slideMasters/") == 2);
    CHECK(m.report.inputs[1].masters_imported == 1);
}

TEST_CASE("merge: --theme=2 puts every slide on deck 2's layouts, with one master") {
    pptctl::Package b;
    REQUIRE(b.open(fixture("merge_b.pptx")));
    const Merged m({fixture("merge_a.pptx"), fixture("merge_b.pptx"), fixture("merge_c.pptx")}, 2);
    CHECK(m.count("ppt/slideMasters/") == 1);
    CHECK(m.count("ppt/slideLayouts/") == 11);
    const auto layouts = m.slide_layouts();
    REQUIRE(layouts.size() == 7);
    for (const auto& layout : layouts) {
        CHECK_MESSAGE(b.part_size(layout), layout);  // one of merge_b's own layouts
    }
    // Same template: each slide keeps the layout type it had.
    CHECK(layouts == std::vector<std::string>{"ppt/slideLayouts/slideLayout1.xml",
                                              "ppt/slideLayouts/slideLayout2.xml",
                                              "ppt/slideLayouts/slideLayout7.xml",
                                              "ppt/slideLayouts/slideLayout2.xml",
                                              "ppt/slideLayouts/slideLayout6.xml",
                                              "ppt/slideLayouts/slideLayout2.xml",
                                              "ppt/slideLayouts/slideLayout6.xml"});
    for (const auto& input : m.report.inputs) {
        CHECK(input.masters_imported == 0);
        CHECK(input.masters_reused == 0);
        CHECK_MESSAGE(input.warnings.empty(), input.path);
    }
    m.check_integrity();
}

TEST_CASE("merge: --theme reports the slides whose layout fell back") {
    ScratchDir dir;
    const auto custom = with_custom_layout(dir.path, "Quarterly Numbers");
    const Merged m({fixture("merge_a.pptx"), custom}, 1);
    CHECK(m.count("ppt/slideMasters/") == 1);
    REQUIRE(m.report.inputs[1].warnings.size() == 1);
    CHECK(
        m.report.inputs[1].warnings[0] ==
        R"(slide 2: deck 1 has no layout like "Quarterly Numbers"; placed on "Title and Content")");
    CHECK(m.slide_layouts().back() == "ppt/slideLayouts/slideLayout2.xml");
    m.check_integrity();
}

TEST_CASE("merge: --theme matches a custom layout by name without a warning") {
    ScratchDir dir;
    const auto custom = with_custom_layout(dir.path, "Title Only");
    const Merged m({fixture("merge_a.pptx"), custom}, 1);
    CHECK(m.report.inputs[1].warnings.empty());
    CHECK(m.slide_layouts().back() == "ppt/slideLayouts/slideLayout6.xml");
}
