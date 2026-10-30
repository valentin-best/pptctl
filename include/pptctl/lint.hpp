// pptctl/lint.hpp
//
// Diagnostic model and rule interface for the lint subcommand.
//
// Stable rule IDs:
//   overflow   — shape geometry extends past slide boundary
//   tiny-text  — text run below minimum font size
//   alt-text   — picture missing alt-text description
//   font-risk  — font referenced but not embedded and not on safe list
//   empty-ph   — placeholder shape contains no text
//   ctype      — suspect content types (printer-settings stubs, etc.)
//   img-bloat  — stored image resolution far exceeds display size
//
// Package-integrity rules (dangling-rel, broken-rid, undeclared-ctype,
// orphan-part, shared-part) check the package itself rather than slide content;
// see integrity.hpp. `--only integrity` selects all of them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <pptctl/presentation.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

namespace pptctl {

enum class Severity : std::uint8_t { Warning, Error };

struct Diagnostic {
    std::string rule_id;  // stable rule identifier, e.g. "overflow"
    Severity severity;
    std::size_t slide_index;  // 1-based
    std::string shape_name;   // empty when not shape-specific
    std::string message;
    // Part the finding is about (integrity rules); empty otherwise. The default
    // initializer is what lets content rules keep their five-field aggregate
    // initializers without -Wmissing-field-initializers.
    std::string part = {};  // NOLINT(readability-redundant-member-init)
};

// Which platform's bundled fonts count as "safe" for the font-risk rule.
// Office-bundled fonts are always safe. All = safe only if present on both
// Windows and macOS (a font that substitutes anywhere is reported).
enum class FontTarget : std::uint8_t { All, Windows, MacOS };

// Output, exit-code and threshold settings of the lint subcommand. Declared
// before LintRule so rules can read their thresholds.
struct LintOptions {
    bool json = false;    // machine-readable JSON instead of compiler-style lines
    bool strict = false;  // exit 2 when any diagnostic is emitted

    // Rule thresholds (CLI: --max-image-dpi, --min-image-bytes, --min-font-size).
    double max_image_dpi = 300.0;              // img-bloat: fire above this effective DPI
    std::uint64_t min_image_bytes = 65'536;    // img-bloat: skip images stored under 64 KiB
    double min_font_pt = 12.0;                 // tiny-text: fire below this size
    FontTarget font_target = FontTarget::All;  // font-risk: --target

    // Rule selection (CLI: --only): rule ids and group aliases ("integrity").
    // Empty runs every rule.
    std::vector<std::string> only;
};

using LintRule = std::function<void(
    const Slide&, const CanvasSize&, const LintOptions&, std::vector<Diagnostic>&)>;

struct RegisteredRule {
    std::string id;
    LintRule fn;
};

}  // namespace pptctl
