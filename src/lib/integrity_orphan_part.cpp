// integrity_orphan_part.cpp — the "orphan-part" rule: parts no relationship
// reaches (#132).

#include <format>
#include <set>
#include <string>
#include <string_view>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

// What the optimize orphans pass (#99) removes.
constexpr std::string_view kMediaDir = "ppt/media/";

}  // namespace

// PartGraph::unreachable(), one warning per part. The .rels of an unreachable
// part goes with it, so it is folded into its owner's finding; a .rels whose
// owner doesn't exist at all has nothing to fold into and is reported itself.
void integrity_orphan_part(const Package& pkg,
                           const PartGraph& graph,
                           const LintOptions& /*opts*/,
                           std::vector<Diagnostic>& out) {
    const auto names = pkg.part_names();
    const std::set<std::string, std::less<>> existing(names.begin(), names.end());
    for (const auto& part : graph.unreachable()) {
        if (part.ends_with('/')) {  // a directory entry, not a part
            continue;
        }
        std::string message;
        if (const auto owner = source_of_rels(part)) {
            if (existing.contains(*owner)) {
                continue;
            }
            message = std::format(
                R"(relationships part "{}" belongs to "{}", which does not exist)", part, *owner);
        } else {
            message = std::format(
                R"(part "{}"{} is not reachable from any relationship{})",
                part,
                existing.contains(rels_part_for(part)) ? " (and its .rels)" : "",
                part.starts_with(kMediaDir) ? "; `pptctl optimize` removes orphan media" : "");
        }
        out.push_back({"orphan-part", Severity::Warning, 0, "", std::move(message), part});
    }
}

}  // namespace pptctl::detail
