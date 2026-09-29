// Diff engine — shape-level ops. See diff_internal.hpp.
//
// Shapes of the two slides are matched level by level (top-level spTree, then
// the children of each matched group pair): by p:cNvPr@id first, then by name,
// then — for placeholders only — by type/idx. Two unrelated text boxes are
// never paired just because they share a kind; that reads as one removed and
// one added, which is what the author did. A matched pair is then compared
// property family by property family, each difference becoming one op.

#include <algorithm>
#include <cstddef>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <pptctl/diff.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diff_internal.hpp"

namespace pptctl {

namespace {

std::string_view kind_label(ShapeKind kind) noexcept {
    switch (kind) {
        case ShapeKind::Placeholder:
            return "placeholder";
        case ShapeKind::TextBox:
            return "textbox";
        case ShapeKind::Picture:
            return "picture";
        case ShapeKind::Table:
            return "table";
        case ShapeKind::Chart:
            return "chart";
        case ShapeKind::Diagram:
            return "diagram";
        case ShapeKind::Ole:
            return "ole";
        case ShapeKind::Group:
            return "group";
        case ShapeKind::Connector:
            return "connector";
        case ShapeKind::Other:
            return "other";
    }
    return "other";
}

double inches(Emu v) noexcept {
    return static_cast<double>(v) / static_cast<double>(kEmuPerInch);
}

bool is_hex6(std::string_view s) noexcept {
    if (s.size() != 6) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    });
}

// "#FF0000" for an sRGB fill, the scheme name otherwise, "no fill" / "inherited"
// for the two non-colour states.
std::string fill_label(const std::optional<Fill>& fill) {
    if (!fill) {
        return "inherited";
    }
    if (fill->kind == "none") {
        return "no fill";
    }
    if (is_hex6(fill->color)) {
        return "#" + fill->color;
    }
    return fill->color.empty() ? std::string{"solid"} : fill->color;
}

// `shape "Name"`, or `shape #id` when the name is empty, plus the kind for
// added/removed shapes and the group path when nested.
std::string subject(const ShapeChange& c) {
    std::string s =
        c.name.empty() ? std::format("shape #{}", c.id) : std::format("shape \"{}\"", c.name);
    if (c.op == ShapeOp::Added || c.op == ShapeOp::Removed) {
        s += std::format(" ({})", kind_label(c.kind));
    }
    if (!c.group_path.empty()) {
        std::string path;
        for (const auto& g : c.group_path) {
            if (!path.empty()) {
                path += '/';
            }
            path += g;
        }
        s += std::format(" in group \"{}\"", path);
    }
    return s;
}

nlohmann::json geometry_json(const std::optional<Geometry>& g) {
    if (!g) {
        return nullptr;
    }
    return {{"x_emu", g->off_x},
            {"y_emu", g->off_y},
            {"cx_emu", g->ext_cx},
            {"cy_emu", g->ext_cy},
            {"x_in", inches(g->off_x)},
            {"y_in", inches(g->off_y)},
            {"cx_in", inches(g->ext_cx)},
            {"cy_in", inches(g->ext_cy)}};
}

nlohmann::json fill_json(const std::optional<Fill>& f) {
    if (!f) {
        return nullptr;
    }
    return {{"kind", f->kind}, {"color", f->color}};
}

}  // namespace

std::string_view to_string(ShapeOp op) noexcept {
    switch (op) {
        case ShapeOp::Added:
            return "added";
        case ShapeOp::Removed:
            return "removed";
        case ShapeOp::Moved:
            return "moved";
        case ShapeOp::Resized:
            return "resized";
        case ShapeOp::KindChanged:
            return "kind_changed";
        case ShapeOp::Recolored:
            return "recolored";
    }
    return "added";
}

std::string describe(const ShapeChange& c) {
    std::string who = subject(c);
    switch (c.op) {
        case ShapeOp::Added:
            if (c.new_geometry) {
                const Geometry& g = *c.new_geometry;
                return std::format("{} added at ({:.2f} in, {:.2f} in), {:.2f} x {:.2f} in",
                                   who,
                                   inches(g.off_x),
                                   inches(g.off_y),
                                   inches(g.ext_cx),
                                   inches(g.ext_cy));
            }
            return std::format("{} added", who);
        case ShapeOp::Removed:
            return std::format("{} removed", who);
        case ShapeOp::Moved:
            return std::format(
                "{} moved by ({:+.2f} in, {:+.2f} in)", who, inches(c.delta_x), inches(c.delta_y));
        case ShapeOp::Resized: {
            std::string s = std::format("{} resized by ({:+.2f} in, {:+.2f} in)",
                                        who,
                                        inches(c.delta_cx),
                                        inches(c.delta_cy));
            if (c.new_geometry) {
                s += std::format(" to {:.2f} x {:.2f} in",
                                 inches(c.new_geometry->ext_cx),
                                 inches(c.new_geometry->ext_cy));
            }
            return s;
        }
        case ShapeOp::KindChanged:
            return std::format("{} changed kind from {} to {}",
                               who,
                               kind_label(c.old_kind.value_or(c.kind)),
                               kind_label(c.kind));
        case ShapeOp::Recolored:
            return std::format(
                "{} recolored from {} to {}", who, fill_label(c.old_fill), fill_label(c.new_fill));
    }
    return who;
}

nlohmann::json to_json(const ShapeChange& c) {
    nlohmann::json j;
    j["op"] = to_string(c.op);
    j["id"] = c.id;
    j["name"] = c.name;
    j["kind"] = kind_label(c.kind);
    j["group_path"] = c.group_path;
    j["old_kind"] = c.old_kind ? nlohmann::json(kind_label(*c.old_kind)) : nullptr;
    j["old_geometry"] = geometry_json(c.old_geometry);
    j["new_geometry"] = geometry_json(c.new_geometry);
    if (c.op == ShapeOp::Moved || c.op == ShapeOp::Resized) {
        j["delta"] = {{"x_emu", c.delta_x},
                      {"y_emu", c.delta_y},
                      {"cx_emu", c.delta_cx},
                      {"cy_emu", c.delta_cy},
                      {"x_in", inches(c.delta_x)},
                      {"y_in", inches(c.delta_y)},
                      {"cx_in", inches(c.delta_cx)},
                      {"cy_in", inches(c.delta_cy)}};
    } else {
        j["delta"] = nullptr;
    }
    j["old_fill"] = fill_json(c.old_fill);
    j["new_fill"] = fill_json(c.new_fill);
    return j;
}

}  // namespace pptctl

namespace pptctl::detail {

namespace {

Emu abs_emu(Emu v) noexcept {
    return v < 0 ? -v : v;
}

bool is_placeholder(const Shape& s) noexcept {
    return s.placeholder_type.has_value() || s.placeholder_idx.has_value();
}

// For each shape of `a`, the index of its counterpart in `b` (or nullopt).
// Three passes, each only over shapes both sides still have unmatched: exact
// id, exact non-empty name, then placeholder kind+type+idx. `b_used` reports
// which shapes of `b` were claimed.
std::vector<std::optional<std::size_t>> match_level(const std::vector<Shape>& a,
                                                    const std::vector<Shape>& b,
                                                    std::vector<bool>& b_used) {
    std::vector<std::optional<std::size_t>> to_b(a.size());
    b_used.assign(b.size(), false);

    const auto pass = [&](auto&& same) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (to_b[i]) {
                continue;
            }
            for (std::size_t j = 0; j < b.size(); ++j) {
                if (!b_used[j] && same(a[i], b[j])) {
                    to_b[i] = j;
                    b_used[j] = true;
                    break;
                }
            }
        }
    };
    pass([](const Shape& x, const Shape& y) { return !x.id.empty() && x.id == y.id; });
    pass([](const Shape& x, const Shape& y) { return !x.name.empty() && x.name == y.name; });
    pass([](const Shape& x, const Shape& y) {
        return is_placeholder(x) && x.kind == y.kind && x.placeholder_type == y.placeholder_type &&
               x.placeholder_idx == y.placeholder_idx;
    });
    return to_b;
}

ShapeChange base_change(ShapeOp op, const Shape& s, const std::vector<std::string>& path) {
    ShapeChange c;
    c.op = op;
    c.id = s.id;
    c.name = s.name;
    c.kind = s.kind;
    c.group_path = path;
    return c;
}

// Ops for one matched pair, in a fixed order: kind, position, size, fill.
void compare_pair(const Shape& x,
                  const Shape& y,
                  const DiffOptions& opts,
                  const std::vector<std::string>& path,
                  std::vector<ShapeChange>& out) {
    if (x.kind != y.kind) {
        ShapeChange c = base_change(ShapeOp::KindChanged, y, path);
        c.old_kind = x.kind;
        c.old_geometry = x.geometry;
        c.new_geometry = y.geometry;
        out.push_back(std::move(c));
    }
    if (x.geometry && y.geometry) {
        const Geometry& g0 = *x.geometry;
        const Geometry& g1 = *y.geometry;
        const Emu tol = opts.geometry_tolerance_emu;
        const Emu dx = g1.off_x - g0.off_x;
        const Emu dy = g1.off_y - g0.off_y;
        if (abs_emu(dx) > tol || abs_emu(dy) > tol) {
            ShapeChange c = base_change(ShapeOp::Moved, y, path);
            c.old_geometry = g0;
            c.new_geometry = g1;
            c.delta_x = dx;
            c.delta_y = dy;
            out.push_back(std::move(c));
        }
        const Emu dcx = g1.ext_cx - g0.ext_cx;
        const Emu dcy = g1.ext_cy - g0.ext_cy;
        if (abs_emu(dcx) > tol || abs_emu(dcy) > tol) {
            ShapeChange c = base_change(ShapeOp::Resized, y, path);
            c.old_geometry = g0;
            c.new_geometry = g1;
            c.delta_cx = dcx;
            c.delta_cy = dcy;
            out.push_back(std::move(c));
        }
    }
    if (x.fill != y.fill) {
        ShapeChange c = base_change(ShapeOp::Recolored, y, path);
        c.old_fill = x.fill;
        c.new_fill = y.fill;
        out.push_back(std::move(c));
    }
}

// Walks one level of the two shape trees, recursing into matched group pairs
// so a child is only ever matched against its parent's counterpart's children.
// NOLINTNEXTLINE(misc-no-recursion)
void diff_level(const std::vector<Shape>& a,
                const std::vector<Shape>& b,
                const DiffOptions& opts,
                std::vector<std::string>& path,
                std::vector<ShapeChange>& out) {
    std::vector<bool> b_used;
    const auto to_b = match_level(a, b, b_used);

    for (std::size_t i = 0; i < a.size(); ++i) {
        const Shape& x = a[i];
        const std::optional<std::size_t> j = to_b[i];
        if (!j) {
            ShapeChange c = base_change(ShapeOp::Removed, x, path);
            c.old_geometry = x.geometry;
            c.old_fill = x.fill;
            out.push_back(std::move(c));
            continue;
        }
        const Shape& y = b[*j];
        compare_pair(x, y, opts, path, out);
        if (x.kind == ShapeKind::Group && y.kind == ShapeKind::Group) {
            path.push_back(y.name);
            diff_level(x.children, y.children, opts, path, out);
            path.pop_back();
        }
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
        if (b_used[j]) {
            continue;
        }
        const Shape& y = b[j];
        ShapeChange c = base_change(ShapeOp::Added, y, path);
        c.new_geometry = y.geometry;
        c.new_fill = y.fill;
        out.push_back(std::move(c));
    }
}

}  // namespace

void diff_shapes(const Slide& a,
                 const Slide& b,
                 const DiffOptions& opts,
                 std::vector<ShapeChange>& out) {
    std::vector<std::string> path;
    diff_level(a.shapes(), b.shapes(), opts, path, out);
}

}  // namespace pptctl::detail
