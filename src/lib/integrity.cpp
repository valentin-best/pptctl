// integrity.cpp — the package-integrity rule registry.

#include <pptctl/integrity.hpp>

#include "integrity_internal.hpp"

namespace pptctl {

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
