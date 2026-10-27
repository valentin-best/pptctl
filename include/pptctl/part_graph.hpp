// pptctl/part_graph.hpp
//
// The package's relationship graph: which parts are reachable from the root
// rels, and which parts reference a given part. One walk shared by every
// optimize pass that needs to know whether a part is still used (orphan media,
// unused layouts/masters, fonts, crop stripping) and by layout/master
// inheritance. Built from `.rels` parts only; part bodies are never parsed.

#pragma once

#include <functional>
#include <map>
#include <pptctl/package.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

// An inbound edge: `source` declares `rel`, whose internal target is the part
// being looked up. An empty source is the package root (`_rels/.rels`).
struct PartReference {
    std::string source;
    Relationship rel;
};

class PartGraph {
 public:
    // Walk `_rels/.rels` and, transitively, the `.rels` of every part reached
    // through an internal relationship. External relationships are not edges.
    // A closed package yields an empty graph.
    [[nodiscard]] static PartGraph build(const Package& package);

    // True if `part` exists in the package and has a path from the root.
    // `[Content_Types].xml` and `.rels` parts are package infrastructure that
    // no relationship targets, so they are not reachable.
    [[nodiscard]] bool is_reachable(std::string_view part) const;

    // Every reachable part, by name.
    [[nodiscard]] const std::set<std::string, std::less<>>& reachable() const noexcept;

    // The references to `part` from reachable sources, in walk order. An
    // unreachable part's references don't count, so a dead slide never keeps
    // its media alive. A dangling target (named by a relationship but absent
    // from the package) still lists the references to it.
    [[nodiscard]] std::span<const PartReference> references_to(std::string_view part) const;

    // Parts with no path from the root, sorted by name: everything except the
    // reachable parts, `[Content_Types].xml` and the `.rels` of the root and of
    // reachable parts. The `.rels` of an unreachable part is itself
    // unreachable, so removing this whole set never strands a relationship.
    [[nodiscard]] const std::vector<std::string>& unreachable() const noexcept;

 private:
    std::set<std::string, std::less<>> reachable_;
    std::map<std::string, std::vector<PartReference>, std::less<>> inbound_;
    std::vector<std::string> unreachable_;
};

}  // namespace pptctl
