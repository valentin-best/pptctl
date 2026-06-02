// pptctl extract — walks slide shape trees and prints their text structure.
//
// This is a minimal human-readable dump. Structured formats (--format
// json|md|txt) and the versioned schema are tracked separately (#12/#13).

#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>

namespace pptctl::cmd {

namespace {

std::string_view kind_label(ShapeKind kind) {
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

std::string human_size(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    constexpr std::array<std::string_view, 3> kUnits{"KB", "MB", "GB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

// The "→ ..." suffix describing where a picture resolves to, plus alt text.
std::string describe_picture(const Picture& pic) {
    std::string out;
    if (pic.linked) {
        out = "→ linked (external)";
    } else if (pic.target.empty()) {
        out = std::format("→ unresolved ({})", pic.rel_id);
    } else {
        out = std::format("→ {}", pic.target);
        std::string detail = pic.content_type;
        if (pic.size_bytes) {
            if (!detail.empty()) {
                detail += ", ";
            }
            detail += human_size(*pic.size_bytes);
        }
        if (!detail.empty()) {
            out += std::format(" ({})", detail);
        }
    }
    if (pic.alt_text) {
        out += std::format(" — {}", *pic.alt_text);
    }
    return out;
}

// Header line + per-series lines for a chart shape.
void print_chart(const Chart& chart, const std::string& pad, std::string_view name) {
    std::string header = std::format("{}[chart] \"{}\"", pad, name);
    if (!chart.chart_type.empty()) {
        header += std::format(" → {}, {} series", chart.chart_type, chart.series.size());
    }
    if (chart.title) {
        header += std::format(" — \"{}\"", *chart.title);
    }
    pptctl::println("{}", header);

    // Format a double as an integer when it has no fractional part. Guards
    // against UB by clamping to long long's representable range first.
    constexpr auto kLLMin = static_cast<double>(std::numeric_limits<long long>::min());
    constexpr auto kLLMax = static_cast<double>(std::numeric_limits<long long>::max());
    auto fmt_val = [&](double v) -> std::string {
        if (v >= kLLMin && v <= kLLMax && v == static_cast<double>(static_cast<long long>(v))) {
            return std::format("{}", static_cast<long long>(v));
        }
        return std::format("{:.6g}", v);
    };

    for (const auto& s : chart.series) {
        std::string line = std::format("{}  \"{}\":", pad, s.name);
        if (!s.categories.empty() && s.categories.size() == s.values.size()) {
            for (std::size_t i = 0; i < s.values.size(); ++i) {
                line += std::format(" {}={}", s.categories[i], fmt_val(s.values[i]));
                if (i + 1 < s.values.size()) {
                    line += ',';
                }
            }
        } else {
            for (std::size_t i = 0; i < s.values.size(); ++i) {
                line += ' ';
                line += fmt_val(s.values[i]);
                if (i + 1 < s.values.size()) {
                    line += ',';
                }
            }
        }
        pptctl::println("{}", line);
    }
}

// One-line-per-row grid summary for a table shape. Cosmetic — the stable
// output formats are tracked in #12.
void print_table(const Table& table, const std::string& pad, std::string_view name) {
    pptctl::println("{}[table] \"{}\" {}x{}", pad, name, table.rows.size(), table.columns);
    for (const auto& row : table.rows) {
        std::string line = "|";
        for (const auto& cell : row.cells) {
            std::string text = cell.text();
            for (auto& ch : text) {
                if (ch == '\n') {
                    ch = ' ';
                }
            }
            line += std::format(" {} |", text);
        }
        pptctl::println("{}  {}", pad, line);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void print_shape(const Shape& shape, int depth) {
    const std::string pad(static_cast<std::size_t>(depth) * 2 + 2, ' ');

    if (!shape.paragraphs.empty()) {
        for (const auto& para : shape.paragraphs) {
            const std::string bullet(static_cast<std::size_t>(para.level) * 2, ' ');
            const std::string text = para.text();
            if (shape.placeholder_type && &para == &shape.paragraphs.front()) {
                pptctl::println("{}[{}] {}", pad, *shape.placeholder_type, text);
            } else if (!text.empty()) {
                pptctl::println("{}{}{}", pad, bullet, text);
            }
        }
    } else if (shape.kind == ShapeKind::Group) {
        pptctl::println("{}[group] \"{}\"", pad, shape.name);
    } else if (shape.picture) {
        pptctl::println("{}[picture] \"{}\" {}", pad, shape.name, describe_picture(*shape.picture));
    } else if (shape.table) {
        print_table(*shape.table, pad, shape.name);
    } else if (shape.chart) {
        print_chart(*shape.chart, pad, shape.name);
    } else {
        pptctl::println("{}[{}] \"{}\"", pad, kind_label(shape.kind), shape.name);
    }

    for (const auto& child : shape.children) {
        print_shape(child, depth + 1);
    }
}

}  // namespace

int extract(const std::string& path) {
    pptctl::Package pkg;
    if (!pkg.open(path)) {
        pptctl::eprintln("error: could not open {}", path);
        return 1;
    }
    pptctl::Presentation pres(pkg);
    if (!pres.load()) {
        pptctl::eprintln("error: {} is not a valid .pptx (no ppt/presentation.xml)", path);
        return 1;
    }

    std::size_t n = 0;
    for (const auto& part : pres.slide_parts()) {
        ++n;
        pptctl::Slide slide;
        if (!slide.load(pkg, part)) {
            pptctl::eprintln("warning: could not read {}", part);
            continue;
        }
        slide.set_index(n);
        pptctl::println("slide {}", slide.index());
        for (const auto& shape : slide.shapes()) {
            print_shape(shape, 0);
        }
        if (!slide.notes().empty()) {
            pptctl::println("  [notes]");
            for (const auto& para : slide.notes()) {
                const std::string text = para.text();
                if (!text.empty()) {
                    pptctl::println("    {}", text);
                }
            }
        }
    }
    return 0;
}

}  // namespace pptctl::cmd
