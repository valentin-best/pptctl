// pptctl lint — static analysis for .pptx files.

#include <cctype>
#include <format>
#include <iostream>
#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

// ── tiny-text rule ────────────────────────────────────────────────────────────

constexpr int kMinFontSizeCentipt = 1200;  // 12 pt

// NOLINTNEXTLINE(misc-no-recursion)
void check_tiny_text_shape(const Shape& shape,
                           std::size_t slide_idx,
                           std::vector<Diagnostic>& out) {
    for (const auto& para : shape.paragraphs) {
        for (const auto& run : para.runs) {
            if (run.props.size_centipt && *run.props.size_centipt < kMinFontSizeCentipt) {
                out.push_back({"tiny-text",
                               Severity::Warning,
                               slide_idx,
                               shape.name,
                               std::format("shape \"{}\" has text at {:.1f}pt (below {}pt minimum)",
                                           shape.name,
                                           run.props.size_pt(),
                                           kMinFontSizeCentipt / 100)});
                return;  // one diagnostic per shape is enough
            }
        }
    }
    for (const auto& child : shape.children) {
        check_tiny_text_shape(child, slide_idx, out);
    }
}

void rule_tiny_text(const Slide& slide,
                    const CanvasSize& /*canvas*/,
                    std::vector<Diagnostic>& out) {
    for (const auto& shape : slide.shapes()) {
        check_tiny_text_shape(shape, slide.index(), out);
    }
}

// ── alt-text rule ─────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-no-recursion)
void check_alt_text_shape(const Shape& shape, std::size_t slide_idx, std::vector<Diagnostic>& out) {
    if (shape.kind == ShapeKind::Picture && shape.picture) {
        const auto& pic = *shape.picture;
        if (!pic.decorative && !pic.alt_text && !pic.title) {
            out.push_back({"alt-text",
                           Severity::Warning,
                           slide_idx,
                           shape.name,
                           std::format("picture \"{}\" has no alt text", shape.name)});
        }
    }
    for (const auto& child : shape.children) {
        check_alt_text_shape(child, slide_idx, out);
    }
}

void rule_alt_text(const Slide& slide, const CanvasSize& /*canvas*/, std::vector<Diagnostic>& out) {
    for (const auto& shape : slide.shapes()) {
        check_alt_text_shape(shape, slide.index(), out);
    }
}

// ── empty-ph rule ─────────────────────────────────────────────────────────────

// Chrome placeholders are field-driven and routinely empty on authored slides;
// flagging them would be all noise.
bool is_chrome_placeholder(std::string_view type) noexcept {
    return type == "sldNum" || type == "dt" || type == "ftr";
}

bool all_whitespace(std::string_view s) noexcept {
    return s.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos;
}

// NOLINTNEXTLINE(misc-no-recursion)
void check_empty_ph_shape(const Shape& shape, std::size_t slide_idx, std::vector<Diagnostic>& out) {
    if (shape.kind == ShapeKind::Placeholder &&
        !is_chrome_placeholder(shape.placeholder_type.value_or(""))) {
        bool has_text = false;
        for (const auto& para : shape.paragraphs) {
            if (!all_whitespace(para.text())) {
                has_text = true;
                break;
            }
        }
        if (!has_text) {
            // Untyped placeholders default to "body" per OOXML.
            const std::string& type = shape.placeholder_type ? *shape.placeholder_type : "body";
            const std::string idx =
                shape.placeholder_idx ? std::format(", idx {}", *shape.placeholder_idx) : "";
            out.push_back(
                {"empty-ph",
                 Severity::Warning,
                 slide_idx,
                 shape.name,
                 std::format("placeholder \"{}\" ({}{}) is empty", shape.name, type, idx)});
        }
    }
    for (const auto& child : shape.children) {
        check_empty_ph_shape(child, slide_idx, out);
    }
}

void rule_empty_ph(const Slide& slide, const CanvasSize& /*canvas*/, std::vector<Diagnostic>& out) {
    for (const auto& shape : slide.shapes()) {
        check_empty_ph_shape(shape, slide.index(), out);
    }
}

// ── font-risk rule ────────────────────────────────────────────────────────────
//
// Deck-level: unlike the per-slide rules above, this one needs every referenced
// face across the whole deck cross-checked against the embedded-font list, so it
// runs once after the slide walk rather than through the per-slide registry.

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Fonts that ship with mainstream platforms / Office, so a bare reference won't
// trigger a silent substitution at render time. Stored lowercased for matching.
const std::set<std::string>& safe_font_list() {
    static const std::set<std::string> kSafe = {"arial",
                                                "arial black",
                                                "arial narrow",
                                                "calibri",
                                                "calibri light",
                                                "cambria",
                                                "cambria math",
                                                "candara",
                                                "comic sans ms",
                                                "consolas",
                                                "constantia",
                                                "corbel",
                                                "courier new",
                                                "georgia",
                                                "impact",
                                                "lucida console",
                                                "lucida sans unicode",
                                                "palatino linotype",
                                                "segoe ui",
                                                "segoe ui light",
                                                "segoe ui semibold",
                                                "symbol",
                                                "tahoma",
                                                "times new roman",
                                                "trebuchet ms",
                                                "verdana",
                                                "webdings",
                                                "wingdings"};
    return kSafe;
}

// A typeface beginning with '+' (e.g. "+mn-lt", "+mj-ea") is a theme-font
// reference, not a literal face: it resolves through the theme, so it is never
// at risk on its own.
bool is_theme_ref(std::string_view tf) noexcept {
    return !tf.empty() && tf.front() == '+';
}

void collect_run_fonts(const RunProperties& p, std::set<std::string>& out) {
    for (const auto* f : {&p.font, &p.font_ea, &p.font_cs}) {
        if (*f && !(*f)->empty() && !is_theme_ref(**f)) {
            out.insert(**f);
        }
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void collect_shape_fonts(const Shape& shape, std::set<std::string>& out) {
    for (const auto& para : shape.paragraphs) {
        for (const auto& run : para.runs) {
            collect_run_fonts(run.props, out);
        }
    }
    for (const auto& child : shape.children) {
        collect_shape_fonts(child, out);
    }
}

// ── ctype rule ────────────────────────────────────────────────────────────────
//
// Deck-level: checks [Content_Types].xml for parts that commonly break
// renderers or that lack a content-type declaration entirely.

const std::set<std::string>& suspect_content_types() {
    static const std::set<std::string> s{
        "application/vnd.ms-office.activeX",
        "application/vnd.ms-office.activeX+xml",
        "application/vnd.ms-office.vbaProject",
        "application/vnd.openxmlformats-officedocument.oleObject",
        "application/vnd.openxmlformats-officedocument.presentationml.printerSettings",
    };
    return s;
}

// Package infrastructure files are not OOXML "parts" and need no content type.
bool is_infrastructure(std::string_view name) noexcept {
    return name == "[Content_Types].xml" || name.ends_with('/') || name.starts_with("_rels/") ||
           name.find("/_rels/") != std::string_view::npos;
}

// ── registry ──────────────────────────────────────────────────────────────────

std::vector<RegisteredRule> make_rules() {
    return {{"overflow", rule_overflow},
            {"tiny-text", rule_tiny_text},
            {"alt-text", rule_alt_text},
            {"empty-ph", rule_empty_ph}};
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

// Every literal typeface referenced by the slide's runs (latin/ea/cs), recursing
// into groups. Theme-font references ("+mn-lt", …) are excluded.
std::set<std::string> slide_referenced_fonts(const Slide& slide) {
    std::set<std::string> out;
    for (const auto& shape : slide.shapes()) {
        collect_shape_fonts(shape, out);
    }
    return out;
}

// One diagnostic per referenced face that is neither embedded in the deck nor on
// the built-in safe list — those substitute silently at render time. Matching is
// case-insensitive; the input set's ordering makes the output deterministic.
std::vector<Diagnostic> check_font_risk(const std::set<std::string>& referenced,
                                        const std::vector<std::string>& embedded) {
    std::set<std::string> embedded_lc;
    for (const auto& e : embedded) {
        embedded_lc.insert(to_lower(e));
    }
    const auto& safe = safe_font_list();
    std::vector<Diagnostic> out;
    for (const auto& face : referenced) {
        const std::string lc = to_lower(face);
        if (safe.contains(lc) || embedded_lc.contains(lc)) {
            continue;
        }
        out.push_back({"font-risk",
                       Severity::Warning,
                       0,  // deck-level: not tied to a single slide
                       "",
                       std::format("font \"{}\" is referenced but not embedded "
                                   "and not on the safe list",
                                   face)});
    }
    return out;
}

// One diagnostic per part whose content type is on the suspect list, plus one per
// part with no declared content type. Infrastructure files ([Content_Types].xml,
// relationship parts) are excluded. Input pairs are (part_name, content_type).
std::vector<Diagnostic> check_ctype(const std::vector<std::pair<std::string, std::string>>& parts) {
    const auto& suspects = suspect_content_types();
    std::vector<Diagnostic> out;
    for (const auto& [part, ct] : parts) {
        if (is_infrastructure(part)) {
            continue;
        }
        if (ct.empty()) {
            out.push_back({"ctype",
                           Severity::Warning,
                           0,
                           "",
                           std::format(R"(part "{}" has no declared content type)", part)});
        } else if (suspects.contains(ct)) {
            out.push_back({"ctype",
                           Severity::Warning,
                           0,
                           "",
                           std::format(R"(part "{}" has suspect content type "{}")", part, ct)});
        }
    }
    return out;
}

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

    std::set<std::string> referenced_fonts;
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
        const auto slide_fonts = slide_referenced_fonts(slide);
        referenced_fonts.insert(slide_fonts.begin(), slide_fonts.end());
    }

    // Theme major/minor faces are the deck's chosen fonts; a custom one that
    // isn't embedded is just as at-risk as one referenced by a run.
    for (const auto& tf : pres.fonts()) {
        if (!tf.empty() && !is_theme_ref(tf)) {
            referenced_fonts.insert(tf);
        }
    }
    const auto font_risks = check_font_risk(referenced_fonts, pres.embedded_fonts());
    diagnostics.insert(diagnostics.end(), font_risks.begin(), font_risks.end());

    std::vector<std::pair<std::string, std::string>> part_ctypes;
    for (const auto& p : pkg.part_names()) {
        part_ctypes.emplace_back(p, pkg.content_type(p));
    }
    const auto ctype_diags = check_ctype(part_ctypes);
    diagnostics.insert(diagnostics.end(), ctype_diags.begin(), ctype_diags.end());

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
