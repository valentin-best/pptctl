// optimize_orphans.cpp — the "orphans" pass: orphan media (#99).
//
// A part under ppt/media/ with no path from the root is dead weight: an editor
// deleted the picture but kept the bytes, or an earlier pass removed the last
// layout or slide that used it. References from unreachable parts don't count
// (that is PartGraph's definition), so media used only by a dead slide goes
// too. Nothing outside ppt/media/ is touched; the pipeline completes each
// removal (the part's .rels, inbound relationships, its content-type Override).

#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "optimize_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kMediaDir = "ppt/media/";

// A media part this pass may remove: under ppt/media/, not a directory entry,
// and not a .rels (which goes with its owner in complete_removals).
bool is_media(std::string_view part) {
    if (!part.starts_with(kMediaDir) || part.ends_with('/')) {
        return false;
    }
    const bool is_rels = part.ends_with(".rels") && part.find("/_rels/") != std::string_view::npos;
    return !is_rels;
}

std::optional<int> hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return std::nullopt;
}

// A part name as OPC compares it: percent-decoded, then ASCII case-folded.
// The graph matches names exactly, so a target spelled "Image1.PNG" or
// "my%20pic.png" misses the part "ppt/media/image1.png" or "ppt/media/my
// pic.png" that renderers would still resolve it to.
std::string comparable(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '%' && i + 2 < name.size()) {
            const auto hi = hex_value(name[i + 1]);
            const auto lo = hex_value(name[i + 2]);
            if (hi && lo) {
                c = static_cast<char>(*hi * 16 + *lo);
                i += 2;
            }
        }
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Every internal target of a live relationship (from the root or a reachable
// part), in comparable form.
std::set<std::string> live_targets(const Package& pkg, const PartGraph& graph) {
    std::set<std::string> targets;
    const auto add_from = [&](std::string_view source) {
        for (const auto& rel : pkg.relationships(source)) {
            if (!rel.external) {
                targets.insert(comparable(rel.target));
            }
        }
    };
    add_from("");
    for (const auto& part : graph.reachable()) {
        add_from(part);
    }
    return targets;
}

}  // namespace

PassResult optimize_orphans(const Package& pkg,
                            const PartGraph& graph,
                            const OptimizeOptions& /*opts*/) {
    std::vector<std::string> candidates;
    for (const auto& part : graph.unreachable()) {
        if (is_media(part)) {
            candidates.push_back(part);
        }
    }
    PassResult result;
    if (candidates.empty()) {
        return result;
    }

    // Keep a part that a live relationship names under another spelling: a
    // renderer resolves it, so it isn't an orphan, whatever the graph says.
    const auto live = live_targets(pkg, graph);
    for (const auto& part : candidates) {
        if (live.contains(comparable(part))) {
            continue;
        }
        result.changes.removed.insert(part);
        result.actions.push_back({part, "removed", "", pkg.part_size(part).value_or(0), 0});
    }
    return result;
}

}  // namespace pptctl::detail
