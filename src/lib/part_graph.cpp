// part_graph.cpp — relationship-graph walk over a package's .rels parts.

#include <cstddef>
#include <pptctl/part_graph.hpp>
#include <utility>

namespace pptctl {

namespace {

constexpr std::string_view kContentTypes = "[Content_Types].xml";

}  // namespace

PartGraph PartGraph::build(const Package& package) {
    PartGraph graph;
    if (!package.is_open()) {
        return graph;
    }
    const auto names = package.part_names();
    const std::set<std::string, std::less<>> existing(names.begin(), names.end());

    // Breadth-first from the root (""), visiting each part's .rels once, so
    // cycles (slide <-> notes slide) terminate and every edge is recorded once.
    std::vector<std::string> queue{""};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const std::string source = queue[i];
        for (auto& rel : package.relationships(source)) {
            if (rel.external) {
                continue;
            }
            const std::string target = rel.target;
            graph.inbound_[target].push_back({source, std::move(rel)});
            if (existing.contains(target) && graph.reachable_.insert(target).second) {
                queue.push_back(target);
            }
        }
    }

    // Only reachable parts are walked, so `inbound_` already holds live edges
    // alone. What remains is everything that isn't reachable or the .rels of
    // something that is.
    std::set<std::string, std::less<>> live_rels{rels_part_for("")};
    for (const auto& part : graph.reachable_) {
        live_rels.insert(rels_part_for(part));
    }
    for (const auto& name : existing) {
        if (name != kContentTypes && !graph.reachable_.contains(name) &&
            !live_rels.contains(name)) {
            graph.unreachable_.push_back(name);
        }
    }
    return graph;
}

bool PartGraph::is_reachable(std::string_view part) const {
    return reachable_.contains(part);
}

const std::set<std::string, std::less<>>& PartGraph::reachable() const noexcept {
    return reachable_;
}

std::span<const PartReference> PartGraph::references_to(std::string_view part) const {
    const auto it = inbound_.find(part);
    if (it == inbound_.end()) {
        return {};
    }
    return it->second;
}

const std::vector<std::string>& PartGraph::unreachable() const noexcept {
    return unreachable_;
}

}  // namespace pptctl
