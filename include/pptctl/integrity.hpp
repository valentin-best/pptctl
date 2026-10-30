// pptctl/integrity.hpp
//
// Package-integrity lint rules: checks of the OPC package itself (parts,
// relationships, content types) rather than of slide content. They read only
// the Package and its PartGraph, so a deck whose presentation.xml won't load
// still gets them, and they are the oracle merge's tests verify output with.
//
// Rules and their ids, in run order (each in src/lib/integrity_<id>.cpp, with
// dashes as underscores):
//   dangling-rel      — an internal relationship targets a part that doesn't exist (#129)
//   broken-rid        — an r:embed / r:id / r:link names an rId the part's .rels lacks (#130)
//   undeclared-ctype  — a part has no content type in [Content_Types].xml (#131)
//   orphan-part       — no relationship reaches a part (#132)
//   shared-part       — a part that must have one owner is targeted from several (#133)
// Every finding carries the part it is about in Diagnostic::part.

#pragma once

#include <functional>
#include <pptctl/lint.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <string>
#include <vector>

namespace pptctl {

using IntegrityRule = std::function<void(
    const Package&, const PartGraph&, const LintOptions&, std::vector<Diagnostic>&)>;

struct RegisteredIntegrityRule {
    std::string id;
    IntegrityRule fn;
};

// The built-in integrity rules, in run order.
[[nodiscard]] const std::vector<RegisteredIntegrityRule>& integrity_rules();

// Run every integrity rule over `package`, in run order.
[[nodiscard]] std::vector<Diagnostic> check_integrity(const Package& package,
                                                      const PartGraph& graph,
                                                      const LintOptions& opts = {});

}  // namespace pptctl
