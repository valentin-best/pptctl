// Tests for the chart-level diff (Phase 4, #74). Owned by the table/chart diff
// issue; keep chart-specific cases here rather than in test_diff.cpp.
//
// Chart data (type, title, series) is only filled by Slide::load() from the
// chart part, so the unit cases drive the per-shape seam in diff_internal.hpp
// with hand-built Shapes, and the end-to-end case loads the charts_deck
// fixture pair through a package.

#include <doctest/doctest.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

#include "../src/lib/diff_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

using pptctl::ChartChange;
using pptctl::ChartOp;
using pptctl::ChartSeries;
using pptctl::Shape;

std::string fixture(const char* name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + name;
}

ChartSeries series(const std::string& name,
                   std::vector<std::string> categories,
                   std::vector<double> values) {
    ChartSeries s;
    s.name = name;
    s.categories = std::move(categories);
    s.values = std::move(values);
    return s;
}

// A chart shape mirroring charts_deck.pptx slide 1: clustered bar, two series
// over Q1..Q3, titled.
Shape bar_chart(const char* id = "2", const char* name = "Chart 1") {
    Shape s;
    s.kind = pptctl::ShapeKind::Chart;
    s.id = id;
    s.name = name;
    pptctl::Chart c;
    c.rel_id = "rId2";
    c.chart_type = "bar";
    c.title = "Revenue vs Cost";
    c.series.push_back(series("Revenue", {"Q1", "Q2", "Q3"}, {100, 200, 150}));
    c.series.push_back(series("Cost", {"Q1", "Q2", "Q3"}, {80, 120, 90}));
    s.chart = std::move(c);
    return s;
}

std::vector<ChartChange> charts_between(const Shape& a, const Shape& b) {
    std::vector<ChartChange> out;
    pptctl::detail::diff_chart_pair(a, b, out);
    return out;
}

// The loaded slide `n` (1-based) of a fixture deck. Both the package and the
// presentation are kept alive by the caller.
pptctl::Slide load_slide(const pptctl::Package& pkg,
                         const pptctl::Presentation& pres,
                         std::size_t n) {
    pptctl::Slide s;
    REQUIRE(s.load(pkg, pres.slide_parts().at(n - 1)));
    s.set_index(n);
    return s;
}

struct OpenDeck {
    pptctl::Package pkg;
    pptctl::Presentation pres{pkg};

    explicit OpenDeck(const char* name) {
        REQUIRE(pkg.open(fixture(name)));
        REQUIRE(pres.load());
    }
};

}  // namespace

// ── unit cases on hand-built shapes ───────────────────────────────────────────

TEST_CASE("Diff/chart: identical charts yield no chart ops") {
    CHECK(charts_between(bar_chart(), bar_chart()).empty());
}

TEST_CASE("Diff/chart: one changed value is one values_changed op with index, old and new") {
    auto b = bar_chart();
    b.chart->series[0].values[1] = 220;
    const auto ops = charts_between(bar_chart(), b);
    REQUIRE(ops.size() == 1);
    const auto& c = ops[0];
    CHECK(c.op == ChartOp::ValuesChanged);
    CHECK(c.shape_id == "2");
    CHECK(c.shape_name == "Chart 1");
    CHECK(c.series == "Revenue");
    CHECK(c.series_index == 1);
    CHECK(c.point == 2);
    CHECK(c.category == "Q2");
    REQUIRE(c.old_value);
    REQUIRE(c.new_value);
    CHECK(*c.old_value == doctest::Approx(200));
    CHECK(*c.new_value == doctest::Approx(220));
}

TEST_CASE("Diff/chart: a point appearing or disappearing has a null side") {
    auto b = bar_chart();
    b.chart->series[1].values.push_back(70);
    b.chart->series[1].categories.push_back("Q4");
    const auto ops = charts_between(bar_chart(), b);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].op == ChartOp::CategoriesChanged);
    CHECK(ops[0].series == "Cost");
    CHECK(ops[0].old_categories == std::vector<std::string>{"Q1", "Q2", "Q3"});
    CHECK(ops[0].new_categories == std::vector<std::string>{"Q1", "Q2", "Q3", "Q4"});
    CHECK(ops[1].op == ChartOp::ValuesChanged);
    CHECK(ops[1].point == 4);
    CHECK(ops[1].category == "Q4");
    CHECK_FALSE(ops[1].old_value);
    REQUIRE(ops[1].new_value);
    CHECK(*ops[1].new_value == doctest::Approx(70));
}

TEST_CASE("Diff/chart: series are matched by name, so a reorder is not a change") {
    auto b = bar_chart();
    std::swap(b.chart->series[0], b.chart->series[1]);
    CHECK(charts_between(bar_chart(), b).empty());
}

TEST_CASE("Diff/chart: a series only in B is series_added; only in A is series_removed") {
    auto b = bar_chart();
    b.chart->series.push_back(series("Profit", {"Q1", "Q2", "Q3"}, {20, 80, 60}));
    const auto added = charts_between(bar_chart(), b);
    REQUIRE(added.size() == 1);
    CHECK(added[0].op == ChartOp::SeriesAdded);
    CHECK(added[0].series == "Profit");
    CHECK(added[0].series_index == 3);

    const auto removed = charts_between(b, bar_chart());
    REQUIRE(removed.size() == 1);
    CHECK(removed[0].op == ChartOp::SeriesRemoved);
    CHECK(removed[0].series == "Profit");
    CHECK(removed[0].series_index == 3);
}

TEST_CASE("Diff/chart: unnamed or duplicate series names fall back to positional matching") {
    auto a = bar_chart();
    auto b = bar_chart();
    a.chart->series[0].name = "";
    b.chart->series[0].name = "";
    b.chart->series[0].values[0] = 101;
    auto ops = charts_between(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == ChartOp::ValuesChanged);
    CHECK(ops[0].series_index == 1);
    CHECK(ops[0].point == 1);

    a = bar_chart();
    b = bar_chart();
    a.chart->series[1].name = "Revenue";
    b.chart->series[1].name = "Revenue";
    b.chart->series[1].values[2] = 91;
    ops = charts_between(a, b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].series_index == 2);
    CHECK(ops[0].point == 3);
}

TEST_CASE("Diff/chart: title and type changes are chart-level ops, emitted first") {
    auto b = bar_chart();
    b.chart->chart_type = "line";
    b.chart->title = "Revenue";
    b.chart->series[0].values[0] = 1;
    const auto ops = charts_between(bar_chart(), b);
    REQUIRE(ops.size() == 3);
    CHECK(ops[0].op == ChartOp::TypeChanged);
    CHECK(ops[0].old_type == "bar");
    CHECK(ops[0].new_type == "line");
    CHECK(ops[0].series_index == 0);
    CHECK(ops[1].op == ChartOp::TitleChanged);
    CHECK(ops[1].old_title == "Revenue vs Cost");
    CHECK(ops[1].new_title == "Revenue");
    CHECK(ops[2].op == ChartOp::ValuesChanged);
}

TEST_CASE("Diff/chart: a title that disappears is title_changed with a null new side") {
    auto b = bar_chart();
    b.chart->title.reset();
    const auto ops = charts_between(bar_chart(), b);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].op == ChartOp::TitleChanged);
    CHECK(ops[0].old_title == "Revenue vs Cost");
    CHECK_FALSE(ops[0].new_title);
}

TEST_CASE("Diff/chart: more than the per-series cap of changed points collapses into one op") {
    auto a = bar_chart();
    auto b = bar_chart();
    const std::size_t n = pptctl::kChartValueOpsPerSeries + 7;
    a.chart->series[0].categories.clear();
    b.chart->series[0].categories.clear();
    a.chart->series[0].values.assign(n, 1.0);
    b.chart->series[0].values.assign(n, 2.0);
    const auto ops = charts_between(a, b);
    REQUIRE(ops.size() == pptctl::kChartValueOpsPerSeries + 1);
    CHECK(ops.back().op == ChartOp::MoreValuesChanged);
    CHECK(ops.back().series == "Revenue");
    CHECK(ops.back().more == 7);
    CHECK(ops[0].category.empty());
}

TEST_CASE("Diff/chart: a chart shape without data on either side yields nothing") {
    Shape a;
    a.kind = pptctl::ShapeKind::Chart;
    a.id = "2";
    a.chart = pptctl::Chart{};
    a.chart->rel_id = "rId2";
    CHECK(charts_between(a, bar_chart()).empty());
    CHECK(charts_between(bar_chart(), a).empty());
}

// ── describe / to_json ────────────────────────────────────────────────────────

TEST_CASE("Diff/chart: describe() gives one readable line per op") {
    ChartChange c;
    c.shape_name = "Chart 1";

    c.op = ChartOp::TypeChanged;
    c.old_type = "bar";
    c.new_type = "line";
    CHECK(describe(c) == R"(chart "Chart 1": type changed: bar -> line)");

    c = ChartChange{};
    c.shape_name = "Chart 1";
    c.op = ChartOp::TitleChanged;
    c.old_title = "Revenue vs Cost";
    CHECK(describe(c) == R"(chart "Chart 1": title changed: "Revenue vs Cost" -> none)");

    c = ChartChange{};
    c.shape_name = "Chart 1";
    c.op = ChartOp::SeriesAdded;
    c.series = "Profit";
    c.series_index = 3;
    CHECK(describe(c) == R"(chart "Chart 1": series "Profit" added)");
    c.op = ChartOp::SeriesRemoved;
    CHECK(describe(c) == R"(chart "Chart 1": series "Profit" removed)");

    c = ChartChange{};
    c.shape_name = "Chart 1";
    c.op = ChartOp::CategoriesChanged;
    c.series = "Revenue";
    c.old_categories = {"Q1", "Q2"};
    c.new_categories = {"Q1", "Q2", "Q3"};
    CHECK(describe(c) ==
          R"(chart "Chart 1": series "Revenue" categories changed: [Q1, Q2] -> [Q1, Q2, Q3])");

    c = ChartChange{};
    c.shape_name = "Chart 1";
    c.op = ChartOp::ValuesChanged;
    c.series = "Revenue";
    c.point = 2;
    c.category = "Q2";
    c.old_value = 200;
    c.new_value = 220.5;
    CHECK(describe(c) == R"(chart "Chart 1": series "Revenue" point 2 (Q2) changed: 200 -> 220.5)");
    c.category.clear();
    c.new_value.reset();
    CHECK(describe(c) == R"(chart "Chart 1": series "Revenue" point 2 changed: 200 -> none)");

    c = ChartChange{};
    c.shape_name = "Chart 1";
    c.op = ChartOp::MoreValuesChanged;
    c.series = "Revenue";
    c.more = 7;
    CHECK(describe(c) == R"(chart "Chart 1": series "Revenue": 7 more points changed)");
}

TEST_CASE("Diff/chart: to_string(ChartOp) is stable snake_case") {
    CHECK(pptctl::to_string(ChartOp::TypeChanged) == "type_changed");
    CHECK(pptctl::to_string(ChartOp::TitleChanged) == "title_changed");
    CHECK(pptctl::to_string(ChartOp::SeriesAdded) == "series_added");
    CHECK(pptctl::to_string(ChartOp::SeriesRemoved) == "series_removed");
    CHECK(pptctl::to_string(ChartOp::CategoriesChanged) == "categories_changed");
    CHECK(pptctl::to_string(ChartOp::ValuesChanged) == "values_changed");
    CHECK(pptctl::to_string(ChartOp::MoreValuesChanged) == "more_values_changed");
}

TEST_CASE("Diff/chart: to_json() carries every key, null when not applicable") {
    ChartChange c;
    c.op = ChartOp::ValuesChanged;
    c.shape_id = "2";
    c.shape_name = "Chart 1";
    c.series = "Revenue";
    c.series_index = 1;
    c.point = 2;
    c.category = "Q2";
    c.old_value = 200;
    c.new_value = 220;
    const auto j = to_json(c);
    const std::vector<std::string> keys = {"op",
                                           "shape_id",
                                           "shape_name",
                                           "series",
                                           "series_index",
                                           "point",
                                           "category",
                                           "old_value",
                                           "new_value",
                                           "old_type",
                                           "new_type",
                                           "old_title",
                                           "new_title",
                                           "old_categories",
                                           "new_categories",
                                           "more"};
    for (const auto& k : keys) {
        CHECK_MESSAGE(j.contains(k), k);
    }
    CHECK(j["op"] == "values_changed");
    CHECK(j["series_index"] == 1);
    CHECK(j["point"] == 2);
    CHECK(j["old_value"] == 200.0);
    CHECK(j["old_type"].is_null());
    CHECK(j["old_title"].is_null());
    CHECK(j["old_categories"].is_null());
    CHECK(j["more"].is_null());

    ChartChange t;
    t.op = ChartOp::TitleChanged;
    t.old_title = "A";
    const auto jt = to_json(t);
    CHECK(jt["old_title"] == "A");
    CHECK(jt["new_title"].is_null());
    CHECK(jt["series"].is_null());
    CHECK(jt["series_index"].is_null());
    CHECK(jt["point"].is_null());
    CHECK(jt["old_value"].is_null());

    ChartChange m;
    m.op = ChartOp::MoreValuesChanged;
    m.series = "Revenue";
    m.series_index = 1;
    m.more = 7;
    CHECK(to_json(m)["more"] == 7);

    ChartChange k;
    k.op = ChartOp::CategoriesChanged;
    k.old_categories = {"Q1"};
    k.new_categories = {"Q1", "Q2"};
    CHECK(to_json(k)["new_categories"] == nlohmann::json::array({"Q1", "Q2"}));
}

// ── end to end on the fixture pair ────────────────────────────────────────────

TEST_CASE("Diff/chart: charts_deck vs charts_deck_v2 yields exactly the expected ops") {
    OpenDeck a("charts_deck.pptx");
    OpenDeck b("charts_deck_v2.pptx");
    REQUIRE(a.pres.slide_parts().size() == 2);
    REQUIRE(b.pres.slide_parts().size() == 2);

    // Slide 1: Revenue Q2 200 -> 220, and a third series "Profit".
    const auto s1 =
        pptctl::compare_slides(load_slide(a.pkg, a.pres, 1), load_slide(b.pkg, b.pres, 1), {});
    CHECK(s1.text.empty());
    CHECK(s1.shapes.empty());
    CHECK(s1.pictures.empty());
    CHECK(s1.tables.empty());
    REQUIRE(s1.charts.size() == 2);
    CHECK(s1.charts[0].op == ChartOp::ValuesChanged);
    CHECK(s1.charts[0].shape_name == "Chart 1");
    CHECK(s1.charts[0].series == "Revenue");
    CHECK(s1.charts[0].point == 2);
    CHECK(s1.charts[0].category == "Q2");
    CHECK(*s1.charts[0].old_value == doctest::Approx(200));
    CHECK(*s1.charts[0].new_value == doctest::Approx(220));
    CHECK(s1.charts[1].op == ChartOp::SeriesAdded);
    CHECK(s1.charts[1].series == "Profit");
    CHECK(s1.charts[1].series_index == 3);

    // Slide 2: the pie chart's title changed.
    const auto s2 =
        pptctl::compare_slides(load_slide(a.pkg, a.pres, 2), load_slide(b.pkg, b.pres, 2), {});
    CHECK(s2.text.empty());
    CHECK(s2.shapes.empty());
    CHECK(s2.tables.empty());
    REQUIRE(s2.charts.size() == 1);
    CHECK(s2.charts[0].op == ChartOp::TitleChanged);
    CHECK(s2.charts[0].old_title == "Market Share");
    CHECK(s2.charts[0].new_title == "Market Share 2025");
}

TEST_CASE("Diff/chart: slide matching sees both charts_deck_v2 slides as changed, not replaced") {
    OpenDeck a("charts_deck.pptx");
    OpenDeck b("charts_deck_v2.pptx");
    std::vector<pptctl::SlideSummary> sa;
    std::vector<pptctl::SlideSummary> sb;
    for (std::size_t n = 1; n <= 2; ++n) {
        sa.push_back(pptctl::summarize(load_slide(a.pkg, a.pres, n)));
        sb.push_back(pptctl::summarize(load_slide(b.pkg, b.pres, n)));
    }
    const auto report = pptctl::match_slides(sa, sb, {});
    REQUIRE(report.slides.size() == 2);
    CHECK(report.slides[0].status == pptctl::SlideStatus::Changed);
    CHECK(report.slides[1].status == pptctl::SlideStatus::Changed);
}
