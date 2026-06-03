// pptctl lint — static analysis for .pptx files.

#include <format>
#include <iostream>
#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::cmd {

namespace {

// ── canvas overflow rule ──────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void check_overflow_shape(const Shape& shape,
                          const CanvasSize& canvas,
                          std::size_t slide_idx,
                          std::vector<Diagnostic>& out) {
    if (shape.geometry) {
        const auto& g = *shape.geometry;
        // Subtraction instead of addition avoids signed int64 overflow UB.
        const bool off = g.off_x < 0 || g.off_y < 0 || g.ext_cx < 0 || g.ext_cy < 0 ||
                         (canvas.width_emu - g.off_x) < g.ext_cx ||
                         (canvas.height_emu - g.off_y) < g.ext_cy;
        if (off) {
            out.push_back({"overflow",
                           Severity::Warning,
                           slide_idx,
                           shape.name,
                           std::format("shape \"{}\" extends past slide boundary", shape.name)});
        }
    }
    for (const auto& child : shape.children) {
        check_overflow_shape(child, canvas, slide_idx, out);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void rule_overflow(const Slide& slide, const CanvasSize& canvas, std::vector<Diagnostic>& out) {
    for (const auto& shape : slide.shapes()) {
        check_overflow_shape(shape, canvas, slide.index(), out);
    }
}

// ── registry ──────────────────────────────────────────────────────────────────

std::vector<RegisteredRule> make_rules() {
    return {{"overflow", rule_overflow}};
}

// ── output ────────────────────────────────────────────────────────────────────

std::string_view severity_label(Severity s) noexcept {
    return s == Severity::Error ? "error" : "warning";
}

void emit_diagnostics(std::ostream& os,
                      const std::string& path,
                      const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) {
        println_to(os,
                   "{}:{}: {}[{}]: {}",
                   path,
                   d.slide_index,
                   severity_label(d.severity),
                   d.rule_id,
                   d.message);
    }
}

}  // namespace

int lint_to(const std::string& path, std::ostream& os) {
    Package pkg;
    if (!pkg.open(path)) {
        eprintln("error: cannot open '{}'", path);
        return 1;
    }
    Presentation pres(pkg);
    if (!pres.load()) {
        eprintln("error: '{}' is not a valid .pptx file", path);
        return 1;
    }

    const auto rules = make_rules();
    const CanvasSize canvas = pres.canvas();
    std::vector<Diagnostic> diagnostics;

    std::size_t n = 0;
    for (const auto& part : pres.slide_parts()) {
        ++n;
        Slide slide;
        if (!slide.load(pkg, part)) {
            eprintln("warning: could not read {}", part);
            continue;
        }
        slide.set_index(n);
        for (const auto& rule : rules) {
            rule.fn(slide, canvas, diagnostics);
        }
    }

    emit_diagnostics(os, path, diagnostics);
    return 0;
}

int lint(const std::string& path) {
    return lint_to(path, std::cout);
}

std::vector<Diagnostic> lint_slide(const Slide& slide, const CanvasSize& canvas) {
    const auto rules = make_rules();
    std::vector<Diagnostic> diagnostics;
    for (const auto& rule : rules) {
        rule.fn(slide, canvas, diagnostics);
    }
    return diagnostics;
}

}  // namespace pptctl::cmd
