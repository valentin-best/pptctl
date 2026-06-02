// pptctl extract — walks slide shape trees and emits their content in the
// requested format: json (default), md, or txt.

#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <nlohmann/json.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>

namespace pptctl::cmd {

namespace {

enum class Format : std::uint8_t { Json, Md, Txt };

Format parse_format(const std::string& s) {
    if (s == "md") {
        return Format::Md;
    }
    if (s == "txt") {
        return Format::Txt;
    }
    return Format::Json;
}

// ── shared helpers ────────────────────────────────────────────────────────────

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

// Format a double as integer when it has no fractional part. Guards against UB
// by clamping to long long's representable range first.
constexpr auto kLLMin = static_cast<double>(std::numeric_limits<long long>::min());
constexpr auto kLLMax = static_cast<double>(std::numeric_limits<long long>::max());
std::string fmt_val(double v) {
    if (v >= kLLMin && v <= kLLMax && v == static_cast<double>(static_cast<long long>(v))) {
        return std::format("{}", static_cast<long long>(v));
    }
    return std::format("{:.6g}", v);
}

// ── txt renderer ──────────────────────────────────────────────────────────────

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

void print_chart(const Chart& chart, const std::string& pad, std::string_view name) {
    std::string header = std::format("{}[chart] \"{}\"", pad, name);
    if (!chart.chart_type.empty()) {
        header += std::format(" → {}, {} series", chart.chart_type, chart.series.size());
    }
    if (chart.title) {
        header += std::format(" — \"{}\"", *chart.title);
    }
    pptctl::println("{}", header);

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

// ── JSON renderer ─────────────────────────────────────────────────────────────

nlohmann::json paragraphs_to_json(const std::vector<Paragraph>& paras) {
    auto arr = nlohmann::json::array();
    for (const auto& p : paras) {
        arr.push_back({{"level", p.level}, {"text", p.text()}});
    }
    return arr;
}

nlohmann::json picture_to_json(const Picture& pic) {
    nlohmann::json j;
    j["linked"] = pic.linked;
    j["target"] = pic.target;
    j["content_type"] = pic.content_type;
    j["size_bytes"] = pic.size_bytes ? nlohmann::json(*pic.size_bytes) : nlohmann::json(nullptr);
    j["alt_text"] = pic.alt_text ? nlohmann::json(*pic.alt_text) : nlohmann::json(nullptr);
    return j;
}

nlohmann::json table_to_json(const Table& table) {
    nlohmann::json j;
    j["columns"] = table.columns;
    auto rows = nlohmann::json::array();
    for (const auto& row : table.rows) {
        auto cells = nlohmann::json::array();
        for (const auto& cell : row.cells) {
            cells.push_back({{"text", cell.text()},
                             {"col_span", cell.col_span},
                             {"row_span", cell.row_span},
                             {"merged", cell.merged}});
        }
        rows.push_back({{"cells", cells}});
    }
    j["rows"] = rows;
    return j;
}

nlohmann::json chart_to_json(const Chart& chart) {
    nlohmann::json j;
    j["type"] = chart.chart_type;
    j["title"] = chart.title ? nlohmann::json(*chart.title) : nlohmann::json(nullptr);
    auto series = nlohmann::json::array();
    for (const auto& s : chart.series) {
        nlohmann::json sj;
        sj["name"] = s.name;
        sj["categories"] = s.categories;
        sj["values"] = s.values;
        series.push_back(sj);
    }
    j["series"] = series;
    return j;
}

// NOLINTNEXTLINE(misc-no-recursion)
nlohmann::json shape_to_json(const Shape& shape) {
    nlohmann::json j;
    j["kind"] = std::string(kind_label(shape.kind));
    j["id"] = shape.id;
    j["name"] = shape.name;
    j["placeholder_type"] =
        shape.placeholder_type ? nlohmann::json(*shape.placeholder_type) : nlohmann::json(nullptr);

    if (shape.geometry) {
        j["geometry"] = {{"off_x", shape.geometry->off_x},
                         {"off_y", shape.geometry->off_y},
                         {"ext_cx", shape.geometry->ext_cx},
                         {"ext_cy", shape.geometry->ext_cy}};
    } else {
        j["geometry"] = nullptr;
    }

    j["paragraphs"] = paragraphs_to_json(shape.paragraphs);
    j["picture"] = shape.picture ? picture_to_json(*shape.picture) : nlohmann::json(nullptr);
    j["table"] = shape.table ? table_to_json(*shape.table) : nlohmann::json(nullptr);
    j["chart"] = shape.chart ? chart_to_json(*shape.chart) : nlohmann::json(nullptr);

    auto children = nlohmann::json::array();
    for (const auto& child : shape.children) {
        children.push_back(shape_to_json(child));
    }
    j["children"] = children;

    return j;
}

// ── Markdown renderer ─────────────────────────────────────────────────────────

// Escape pipe characters so they don't break GFM table cells.
std::string md_escape_cell(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch == '|') {
            out += "\\|";
        } else if (ch == '\n') {
            out += ' ';
        } else {
            out += ch;
        }
    }
    return out;
}

void md_print_table(const Table& table) {
    if (table.rows.empty()) {
        return;
    }

    // Header row (first row)
    {
        std::string line = "|";
        for (const auto& cell : table.rows.front().cells) {
            line += ' ';
            line += md_escape_cell(cell.text());
            line += " |";
        }
        pptctl::println("{}", line);
    }

    // Separator
    {
        std::string sep = "|";
        for (std::size_t i = 0; i < table.columns; ++i) {
            sep += "---|";
        }
        pptctl::println("{}", sep);
    }

    // Data rows
    for (std::size_t r = 1; r < table.rows.size(); ++r) {
        std::string line = "|";
        for (const auto& cell : table.rows[r].cells) {
            line += ' ';
            line += md_escape_cell(cell.text());
            line += " |";
        }
        pptctl::println("{}", line);
    }
}

void md_print_chart(const Chart& chart, std::string_view name) {
    std::string header = std::format("- [chart] \"{}\"", name);
    if (!chart.chart_type.empty()) {
        header += std::format(" — {}", chart.chart_type);
    }
    if (chart.title) {
        header += std::format(" — \"{}\"", *chart.title);
    }
    pptctl::println("{}", header);

    for (const auto& s : chart.series) {
        std::string line = std::format("  - \"{}\":", s.name);
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

// NOLINTNEXTLINE(misc-no-recursion)
void md_print_shape(const Shape& shape, int depth) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');

    if (!shape.paragraphs.empty()) {
        for (const auto& para : shape.paragraphs) {
            const std::string text = para.text();
            if (text.empty()) {
                continue;
            }
            const std::string bullet_indent(static_cast<std::size_t>(para.level) * 2, ' ');
            if (shape.placeholder_type && &para == &shape.paragraphs.front()) {
                pptctl::println(
                    "{}{}- **[{}]** {}", indent, bullet_indent, *shape.placeholder_type, text);
            } else {
                pptctl::println("{}{}- {}", indent, bullet_indent, text);
            }
        }
    } else if (shape.picture) {
        std::string line = std::format("{}- [picture] \"{}\"", indent, shape.name);
        if (!shape.picture->target.empty()) {
            line += std::format(" → {}", shape.picture->target);
        } else if (shape.picture->linked) {
            line += " → linked (external)";
        }
        if (shape.picture->alt_text) {
            line += std::format(" — {}", *shape.picture->alt_text);
        }
        pptctl::println("{}", line);
    } else if (shape.table) {
        md_print_table(*shape.table);
    } else if (shape.chart) {
        md_print_chart(*shape.chart, shape.name);
    } else if (shape.kind == ShapeKind::Group) {
        pptctl::println("{}- [group] \"{}\"", indent, shape.name);
    } else {
        pptctl::println("{}- [{}] \"{}\"", indent, kind_label(shape.kind), shape.name);
    }

    for (const auto& child : shape.children) {
        md_print_shape(child, depth + 1);
    }
}

}  // namespace

int extract(const std::string& path, const std::string& format_str) {
    const Format fmt = parse_format(format_str);

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

    if (fmt == Format::Json) {
        nlohmann::json out;
        out["schema_version"] = 1;
        out["file"] = path;
        out["slide_count"] = pres.slide_count();
        auto slides = nlohmann::json::array();

        std::size_t n = 0;
        for (const auto& part : pres.slide_parts()) {
            ++n;
            pptctl::Slide slide;
            if (!slide.load(pkg, part)) {
                pptctl::eprintln("warning: could not read {}", part);
                continue;
            }
            slide.set_index(n);

            nlohmann::json sj;
            sj["index"] = slide.index();
            auto shapes = nlohmann::json::array();
            for (const auto& shape : slide.shapes()) {
                shapes.push_back(shape_to_json(shape));
            }
            sj["shapes"] = shapes;
            sj["notes"] = slide.notes_text();
            slides.push_back(sj);
        }
        out["slides"] = slides;
        pptctl::println("{}", out.dump(2));
        return 0;
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

        if (fmt == Format::Md) {
            if (n > 1) {
                pptctl::println("");
            }
            pptctl::println("## Slide {}", slide.index());
            pptctl::println("");
            for (const auto& shape : slide.shapes()) {
                md_print_shape(shape, 0);
            }
            const std::string notes = slide.notes_text();
            if (!notes.empty()) {
                pptctl::println("");
                pptctl::println("> Notes: {}", notes);
            }
        } else {
            // txt
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
    }
    return 0;
}

}  // namespace pptctl::cmd
