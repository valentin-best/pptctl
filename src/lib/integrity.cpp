// integrity.cpp — the package-integrity rule registry.

#include <optional>
#include <pptctl/integrity.hpp>
#include <string>
#include <string_view>

#include "integrity_internal.hpp"

namespace pptctl {

namespace {

constexpr std::string_view kRelsDir = "_rels/";
constexpr std::string_view kRelsExt = ".rels";

}  // namespace

std::optional<std::string> detail::source_of_rels(std::string_view name) {
    if (!name.ends_with(kRelsExt)) {
        return std::nullopt;
    }
    const auto slash = name.rfind('/');
    const std::string_view dir = slash == std::string_view::npos ? "" : name.substr(0, slash + 1);
    if (!dir.ends_with(kRelsDir)) {
        return std::nullopt;
    }
    const std::string_view file =
        name.substr(dir.size(), name.size() - dir.size() - kRelsExt.size());
    std::string source(dir.substr(0, dir.size() - kRelsDir.size()));
    source += file;
    return source;
}

const std::vector<RegisteredIntegrityRule>& integrity_rules() {
    static const std::vector<RegisteredIntegrityRule> rules = {
        {"dangling-rel", detail::integrity_dangling_rel},
        {"broken-rid", detail::integrity_broken_rid},
        {"undeclared-ctype", detail::integrity_undeclared_ctype},
        {"orphan-part", detail::integrity_orphan_part},
        {"shared-part", detail::integrity_shared_part},
    };
    return rules;
}

std::vector<Diagnostic> check_integrity(const Package& package,
                                        const PartGraph& graph,
                                        const LintOptions& opts) {
    std::vector<Diagnostic> out;
    for (const auto& rule : integrity_rules()) {
        rule.fn(package, graph, opts, out);
    }
    return out;
}

}  // namespace pptctl
