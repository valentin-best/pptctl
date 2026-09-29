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
};

using LintRule = std::function<void(const Slide&, const CanvasSize&, std::vector<Diagnostic>&)>;

struct RegisteredRule {
    std::string id;
    LintRule fn;
};

// Output / exit-code behaviour of the lint subcommand.
struct LintOptions {
    bool json = false;    // machine-readable JSON instead of compiler-style lines
    bool strict = false;  // exit 2 when any diagnostic is emitted
};

}  // namespace pptctl
