// pptctl extract — walks slide shape trees and prints their text structure.
//
// This is a minimal human-readable dump. Structured formats (--format
// json|md|txt) and the versioned schema are tracked separately (#12/#13).

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
    }
    return 0;
}

}  // namespace pptctl::cmd
