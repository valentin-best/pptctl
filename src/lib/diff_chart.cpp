// Diff engine — chart-level ops. See diff_internal.hpp.
//
// Chart shapes are paired by shape id, then by name (groups walked). Each
// pair is compared on chart type and title, then series are matched by name
// (when every name on both sides is distinct and non-empty) or by position,
// and each matched series is compared on its category labels and, point by
// point, its values. Values are compared exactly: both sides were parsed
// from the same XML text form, so equal text gives equal doubles.

#include <cstddef>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

// ── matching ─────────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void collect_charts(const std::vector<Shape>& shapes, std::vector<const Shape*>& out) {
    for (const auto& s : shapes) {
        if (s.kind == ShapeKind::Chart && s.chart) {
            out.push_back(&s);
        }
        if (!s.children.empty()) {
            collect_charts(s.children, out);
        }
    }
}

struct Match {
    const Shape* a;
    const Shape* b;
};

// Pairs A's charts with B's: first by shape id, then by name among the
// leftovers (first come, first served, in document order). Unpaired charts
// are not reported here — the shape diff lists them as added/removed.
std::vector<Match> match_charts(const std::vector<const Shape*>& a,
                                const std::vector<const Shape*>& b) {
    std::vector<bool> b_used(b.size(), false);
    std::vector<const Shape*> a_partner(a.size(), nullptr);

    const auto claim = [&](std::size_t i, auto key) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (!b_used[j] && key(*b[j]) == key(*a[i])) {
                b_used[j] = true;
                a_partner[i] = b[j];
                return;
            }
        }
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        claim(i, [](const Shape& s) -> const std::string& { return s.id; });
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a_partner[i] == nullptr && !a[i]->name.empty()) {
            claim(i, [](const Shape& s) -> const std::string& { return s.name; });
        }
    }

    std::vector<Match> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a_partner[i] != nullptr) {
            out.push_back({a[i], a_partner[i]});
        }
    }
    return out;
}

// True when every series has a non-empty name and no two share one, so the
// names can serve as identities.
bool names_are_keys(const std::vector<ChartSeries>& series) {
    std::unordered_set<std::string_view> seen;
    for (const auto& s : series) {
        if (s.name.empty() || !seen.insert(s.name).second) {
            return false;
        }
    }
    return true;
}

// Series pairs as (A index, B index); npos on the missing side. In B order
// for matched and added series, with removed A series appended in A order.
struct SeriesMatch {
    std::size_t a;
    std::size_t b;
};

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::vector<SeriesMatch> match_series(const std::vector<ChartSeries>& a,
                                      const std::vector<ChartSeries>& b) {
    std::vector<SeriesMatch> out;
    if (names_are_keys(a) && names_are_keys(b)) {
        std::vector<bool> a_used(a.size(), false);
        for (std::size_t j = 0; j < b.size(); ++j) {
            std::size_t found = kNone;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i].name == b[j].name) {
                    found = i;
                    a_used[i] = true;
                    break;
                }
            }
            out.push_back({found, j});
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!a_used[i]) {
                out.push_back({i, kNone});
            }
        }
        return out;
    }
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t k = 0; k < common; ++k) {
        out.push_back({k, k});
    }
    for (std::size_t j = common; j < b.size(); ++j) {
        out.push_back({kNone, j});
    }
    for (std::size_t i = common; i < a.size(); ++i) {
        out.push_back({i, kNone});
    }
    return out;
}

// ── comparison ───────────────────────────────────────────────────────────────

struct Owner {
    std::string shape_id;
    std::string shape_name;
};

ChartChange make_change(const Owner& owner, ChartOp op) {
    ChartChange c;
    c.op = op;
    c.shape_id = owner.shape_id;
    c.shape_name = owner.shape_name;
    return c;
}

ChartChange series_change(const Owner& owner, ChartOp op, const ChartSeries& s, std::size_t index) {
    auto c = make_change(owner, op);
    c.series = s.name;
    c.series_index = index;
    return c;
}

void diff_series(const Owner& owner,
                 const ChartSeries& a,
                 const ChartSeries& b,
                 std::size_t b_index,
                 std::vector<ChartChange>& out) {
    if (a.categories != b.categories) {
        auto c = series_change(owner, ChartOp::CategoriesChanged, b, b_index);
        c.old_categories = a.categories;
        c.new_categories = b.categories;
        out.push_back(std::move(c));
    }
    const std::size_t n = std::max(a.values.size(), b.values.size());
    std::size_t emitted = 0;
    std::size_t more = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const bool in_a = k < a.values.size();
        const bool in_b = k < b.values.size();
        if (in_a && in_b && a.values[k] == b.values[k]) {
            continue;
        }
        if (emitted == kChartValueOpsPerSeries) {
            ++more;
            continue;
        }
        auto c = series_change(owner, ChartOp::ValuesChanged, b, b_index);
        c.point = k + 1;
        if (k < b.categories.size()) {
            c.category = b.categories[k];
        } else if (k < a.categories.size()) {
            c.category = a.categories[k];
        }
        if (in_a) {
            c.old_value = a.values[k];
        }
        if (in_b) {
            c.new_value = b.values[k];
        }
        out.push_back(std::move(c));
        ++emitted;
    }
    if (more > 0) {
        auto c = series_change(owner, ChartOp::MoreValuesChanged, b, b_index);
        c.more = more;
        out.push_back(std::move(c));
    }
}

// ── rendering ────────────────────────────────────────────────────────────────

std::string value_label(const std::optional<double>& v) {
    return v ? std::format("{}", *v) : std::string("none");
}

std::string title_label(const std::optional<std::string>& t) {
    return t ? std::format("\"{}\"", *t) : std::string("none");
}

std::string list_label(const std::vector<std::string>& v) {
    std::string out = "[";
    for (const auto& s : v) {
        if (out.size() > 1) {
            out += ", ";
        }
        out += s;
    }
    return out + "]";
}

nlohmann::json opt_json(const std::optional<double>& v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}

nlohmann::json opt_json(const std::optional<std::string>& v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}

bool is_series_op(ChartOp op) noexcept {
    return op != ChartOp::TypeChanged && op != ChartOp::TitleChanged;
}

}  // namespace

namespace detail {

void diff_chart_pair(const Shape& a, const Shape& b, std::vector<ChartChange>& out) {
    if (!a.chart || !b.chart) {
        return;
    }
    const Chart& ca = *a.chart;
    const Chart& cb = *b.chart;
    // A parse-only chart (no package) has no type, title or series: nothing
    // to compare against, and nothing to report.
    if (ca.chart_type.empty() || cb.chart_type.empty()) {
        return;
    }
    const Owner owner{b.id, b.name};

    if (ca.chart_type != cb.chart_type) {
        auto c = make_change(owner, ChartOp::TypeChanged);
        c.old_type = ca.chart_type;
        c.new_type = cb.chart_type;
        out.push_back(std::move(c));
    }
    if (ca.title != cb.title) {
        auto c = make_change(owner, ChartOp::TitleChanged);
        c.old_title = ca.title;
        c.new_title = cb.title;
        out.push_back(std::move(c));
    }
    for (const auto& m : match_series(ca.series, cb.series)) {
        if (m.a == kNone) {
            out.push_back(series_change(owner, ChartOp::SeriesAdded, cb.series[m.b], m.b + 1));
        } else if (m.b == kNone) {
            out.push_back(series_change(owner, ChartOp::SeriesRemoved, ca.series[m.a], m.a + 1));
        } else {
            diff_series(owner, ca.series[m.a], cb.series[m.b], m.b + 1, out);
        }
    }
}

void diff_charts(const Slide& a,
                 const Slide& b,
                 const DiffOptions& /*opts*/,
                 std::vector<ChartChange>& out) {
    std::vector<const Shape*> ca;
    std::vector<const Shape*> cb;
    collect_charts(a.shapes(), ca);
    collect_charts(b.shapes(), cb);
    for (const auto& m : match_charts(ca, cb)) {
        diff_chart_pair(*m.a, *m.b, out);
    }
}

}  // namespace detail

std::string_view to_string(ChartOp op) noexcept {
    switch (op) {
        case ChartOp::TypeChanged:
            return "type_changed";
        case ChartOp::TitleChanged:
            return "title_changed";
        case ChartOp::SeriesAdded:
            return "series_added";
        case ChartOp::SeriesRemoved:
            return "series_removed";
        case ChartOp::CategoriesChanged:
            return "categories_changed";
        case ChartOp::ValuesChanged:
            return "values_changed";
        case ChartOp::MoreValuesChanged:
            return "more_values_changed";
    }
    return "unknown";
}

std::string describe(const ChartChange& change) {
    std::string head = std::format("chart \"{}\"", change.shape_name);
    const std::string series = std::format("series \"{}\"", change.series);
    switch (change.op) {
        case ChartOp::TypeChanged:
            return std::format(
                "{}: type changed: {} -> {}", head, change.old_type, change.new_type);
        case ChartOp::TitleChanged:
            return std::format("{}: title changed: {} -> {}",
                               head,
                               title_label(change.old_title),
                               title_label(change.new_title));
        case ChartOp::SeriesAdded:
            return std::format("{}: {} added", head, series);
        case ChartOp::SeriesRemoved:
            return std::format("{}: {} removed", head, series);
        case ChartOp::CategoriesChanged:
            return std::format("{}: {} categories changed: {} -> {}",
                               head,
                               series,
                               list_label(change.old_categories),
                               list_label(change.new_categories));
        case ChartOp::ValuesChanged:
            return std::format("{}: {} point {}{} changed: {} -> {}",
                               head,
                               series,
                               change.point,
                               change.category.empty() ? "" : std::format(" ({})", change.category),
                               value_label(change.old_value),
                               value_label(change.new_value));
        case ChartOp::MoreValuesChanged:
            return std::format("{}: {}: {} more points changed", head, series, change.more);
    }
    return head;
}

nlohmann::json to_json(const ChartChange& change) {
    const bool series = is_series_op(change.op);
    const bool values = change.op == ChartOp::ValuesChanged;
    nlohmann::json j;
    j["op"] = to_string(change.op);
    j["shape_id"] = change.shape_id;
    j["shape_name"] = change.shape_name;
    j["series"] = series ? nlohmann::json(change.series) : nlohmann::json(nullptr);
    j["series_index"] = series ? nlohmann::json(change.series_index) : nlohmann::json(nullptr);
    j["point"] = values ? nlohmann::json(change.point) : nlohmann::json(nullptr);
    j["category"] = values ? nlohmann::json(change.category) : nlohmann::json(nullptr);
    j["old_value"] = values ? opt_json(change.old_value) : nlohmann::json(nullptr);
    j["new_value"] = values ? opt_json(change.new_value) : nlohmann::json(nullptr);
    j["old_type"] = change.op == ChartOp::TypeChanged ? nlohmann::json(change.old_type)
                                                      : nlohmann::json(nullptr);
    j["new_type"] = change.op == ChartOp::TypeChanged ? nlohmann::json(change.new_type)
                                                      : nlohmann::json(nullptr);
    j["old_title"] =
        change.op == ChartOp::TitleChanged ? opt_json(change.old_title) : nlohmann::json(nullptr);
    j["new_title"] =
        change.op == ChartOp::TitleChanged ? opt_json(change.new_title) : nlohmann::json(nullptr);
    j["old_categories"] = change.op == ChartOp::CategoriesChanged
                              ? nlohmann::json(change.old_categories)
                              : nlohmann::json(nullptr);
    j["new_categories"] = change.op == ChartOp::CategoriesChanged
                              ? nlohmann::json(change.new_categories)
                              : nlohmann::json(nullptr);
    j["more"] = change.op == ChartOp::MoreValuesChanged ? nlohmann::json(change.more)
                                                        : nlohmann::json(nullptr);
    return j;
}

}  // namespace pptctl
