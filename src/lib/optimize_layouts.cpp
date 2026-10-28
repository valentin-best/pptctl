// optimize_layouts.cpp — the "layouts" pass: unused slide layouts and masters (#100).
//
// A slide layout is used when a reachable part other than a slide master
// (in practice, a slide) has a relationship to it. A slide master is used when
// one of its layouts is, or when anything other than the presentation and its
// own layouts points at it. Unused layouts and masters are removed, along with
// every part that was reachable only through them: a removed master's theme
// (unless the presentation or another master still uses it) and any media,
// tags or other parts only a removed layout/master pointed at. Removing that
// cascade here rather than leaving media to the orphans pass keeps the result
// complete under --no-orphans and attributes the bytes to this pass.
//
// Notes masters and handout masters hang off the presentation, never off a
// slide master, so they and their themes are never part of the cascade.
//
// Three rules keep the output a valid deck:
//   * the deck never loses its last master: one with no used layout at all
//     (no slides) keeps its first master in p:sldMasterIdLst order;
//   * a master that stays without a used layout (that one, or a master
//     something else points at) keeps its first layout in p:sldLayoutIdLst
//     order, so no master is left with no layouts;
//   * if presentation.xml or a master that needs editing can't be parsed, the
//     pass changes nothing.
//
// The pass rewrites only two kinds of part bodies, and only when an entry in
// them goes: p:sldLayoutIdLst in a surviving master and p:sldMasterIdLst in
// presentation.xml. Surviving entries keep their ids and r:ids. The .rels and
// [Content_Types].xml bookkeeping is the pipeline's (complete_removals).

#include <algorithm>
#include <cstddef>
#include <map>
#include <pugixml.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "optimize_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kLayoutsDir = "ppt/slideLayouts/";
constexpr std::string_view kMastersDir = "ppt/slideMasters/";
constexpr std::string_view kThemeDir = "ppt/theme/";
constexpr std::string_view kMediaDir = "ppt/media/";

// Relationship types compared by their last segment, so transitional and
// strict OOXML namespaces both match.
bool has_type(const Relationship& rel, std::string_view name) {
    const auto slash = rel.type.rfind('/');
    return !rel.external && slash != std::string::npos &&
           std::string_view(rel.type).substr(slash + 1) == name;
}

// The local part of an element or attribute name: "p:sldId" -> "sldId".
std::string_view local_name(std::string_view name) {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node child_named(const pugi::xml_node& parent, std::string_view local) {
    for (auto child : parent.children()) {
        if (child.type() == pugi::node_element && local_name(child.name()) == local) {
            return child;
        }
    }
    return {};
}

// The relationship id of an id-list entry: its prefixed `id` attribute
// (r:id), as opposed to the unprefixed numeric `id`.
std::string_view entry_rid(const pugi::xml_node& entry) {
    for (auto attr : entry.attributes()) {
        const std::string_view name = attr.name();
        if (name.find(':') != std::string_view::npos && local_name(name) == "id") {
            return attr.value();
        }
    }
    return {};
}

bool load_part(const Package& pkg, std::string_view part, pugi::xml_document& doc) {
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    const auto bytes = pkg.read_part(part);
    return !bytes.empty() &&
           static_cast<bool>(
               doc.load_buffer(bytes.data(), bytes.size(), flags, pugi::encoding_utf8)) &&
           doc.document_element();
}

// Serialize the way package.cpp's rewrite_rels does: raw, declaration kept.
std::vector<std::byte> save_part(const pugi::xml_document& doc) {
    struct BytesWriter : pugi::xml_writer {
        std::vector<std::byte> out;
        void write(const void* data, std::size_t size) override {
            const auto* p = static_cast<const std::byte*>(data);
            out.insert(out.end(), p, p + size);
        }
    } writer;
    doc.save(writer, "", pugi::format_raw, pugi::encoding_utf8);
    return std::move(writer.out);
}

// An id list (`list_name` under the document element of `part`), read as the
// targets of its entries in document order.
struct IdList {
    pugi::xml_document doc;
    pugi::xml_node list;
    std::vector<std::string> targets;  // resolved target per entry; empty if unresolvable
};

bool read_id_list(const Package& pkg,
                  const std::string& part,
                  std::string_view list_name,
                  IdList& out) {
    if (!load_part(pkg, part, out.doc)) {
        return false;
    }
    out.list = child_named(out.doc.document_element(), list_name);
    for (auto entry : out.list.children()) {
        if (entry.type() == pugi::node_element) {
            out.targets.push_back(pkg.resolve(part, entry_rid(entry)));
        }
    }
    return true;
}

// Drop the entries of `ids` whose target is in `removed`. True if any went.
bool drop_entries(IdList& ids, const std::set<std::string>& removed) {
    bool dropped = false;
    std::size_t i = 0;
    for (auto entry = ids.list.first_child(); entry;) {
        auto next = entry.next_sibling();
        if (entry.type() == pugi::node_element) {
            if (removed.contains(ids.targets[i])) {
                ids.list.remove_child(entry);
                dropped = true;
            }
            ++i;
        }
        entry = next;
    }
    return dropped;
}

// The package's main part: the root's officeDocument relationship.
std::string main_part(const Package& pkg) {
    for (const auto& rel : pkg.relationships("")) {
        if (has_type(rel, "officeDocument")) {
            return rel.target;
        }
    }
    return {};
}

// Parts reachable from the root without passing through `removed`.
std::set<std::string> reachable_without(const Package& pkg, const std::set<std::string>& removed) {
    std::set<std::string> seen;
    std::vector<std::string> stack{""};
    while (!stack.empty()) {
        auto source = std::move(stack.back());
        stack.pop_back();
        for (const auto& rel : pkg.relationships(source)) {
            if (rel.external || removed.contains(rel.target) || seen.contains(rel.target)) {
                continue;
            }
            seen.insert(rel.target);
            stack.push_back(rel.target);
        }
    }
    return seen;
}

std::string detail_for(std::string_view part) {
    if (part.starts_with(kLayoutsDir)) {
        return "layout";
    }
    if (part.starts_with(kMastersDir)) {
        return "master";
    }
    if (part.starts_with(kThemeDir)) {
        return "theme";
    }
    if (part.starts_with(kMediaDir)) {
        return "media";
    }
    return "only used by removed layouts";
}

int detail_rank(std::string_view detail) {
    if (detail == "layout") {
        return 0;
    }
    return detail == "master" ? 1 : 2;
}

}  // namespace

PassResult optimize_layouts(const Package& pkg,
                            const PartGraph& graph,
                            const OptimizeOptions& /*opts*/) {
    const auto pres_part = main_part(pkg);
    if (pres_part.empty() || !graph.is_reachable(pres_part)) {
        return {};
    }
    IdList pres;
    if (!read_id_list(pkg, pres_part, "sldMasterIdLst", pres)) {
        return {};
    }

    // Masters in p:sldMasterIdLst order, then any the presentation's rels
    // declare without listing them (kept in rels order).
    std::vector<std::string> masters;
    for (const auto& target : pres.targets) {
        if (!target.empty() && graph.is_reachable(target) &&
            std::find(masters.begin(), masters.end(), target) == masters.end()) {
            masters.push_back(target);
        }
    }
    for (const auto& rel : pkg.relationships(pres_part)) {
        if (has_type(rel, "slideMaster") && graph.is_reachable(rel.target) &&
            std::find(masters.begin(), masters.end(), rel.target) == masters.end()) {
            masters.push_back(rel.target);
        }
    }
    if (masters.empty()) {
        return {};
    }
    const std::set<std::string> master_set(masters.begin(), masters.end());

    // Each master's layouts (rels order), and which layouts something other
    // than a master uses.
    std::map<std::string, std::vector<std::string>> layouts_of;
    std::set<std::string> used;
    for (const auto& master : masters) {
        auto& layouts = layouts_of[master];
        for (const auto& rel : pkg.relationships(master)) {
            if (!has_type(rel, "slideLayout") || !graph.is_reachable(rel.target)) {
                continue;
            }
            layouts.push_back(rel.target);
            const auto refs = graph.references_to(rel.target);
            if (std::any_of(refs.begin(), refs.end(), [&](const PartReference& ref) {
                    return !master_set.contains(ref.source);
                })) {
                used.insert(rel.target);
            }
        }
    }

    // A master goes when none of its layouts is used and nothing but the
    // presentation and its own layouts points at it, but never the last one.
    std::vector<std::string> removed_masters;
    for (const auto& master : masters) {
        const auto& layouts = layouts_of[master];
        const bool any_used = std::any_of(
            layouts.begin(), layouts.end(), [&](const std::string& l) { return used.contains(l); });
        const auto refs = graph.references_to(master);
        const bool held_elsewhere =
            std::any_of(refs.begin(), refs.end(), [&](const PartReference& ref) {
                return ref.source != pres_part &&
                       std::find(layouts.begin(), layouts.end(), ref.source) == layouts.end();
            });
        if (!any_used && !held_elsewhere) {
            removed_masters.push_back(master);
        }
    }
    if (removed_masters.size() == masters.size()) {
        removed_masters.erase(removed_masters.begin());  // a deck with no used layout at all
    }

    // A surviving master keeps at least one layout: its first listed one.
    for (const auto& master : masters) {
        const auto& layouts = layouts_of[master];
        if (std::find(removed_masters.begin(), removed_masters.end(), master) !=
                removed_masters.end() ||
            layouts.empty() || std::any_of(layouts.begin(), layouts.end(), [&](const auto& l) {
                return used.contains(l);
            })) {
            continue;
        }
        IdList ids;
        if (!read_id_list(pkg, master, "sldLayoutIdLst", ids)) {
            return {};
        }
        const auto listed =
            std::find_if(ids.targets.begin(), ids.targets.end(), [&](const auto& t) {
                return std::find(layouts.begin(), layouts.end(), t) != layouts.end();
            });
        used.insert(listed != ids.targets.end() ? *listed : layouts.front());
    }

    std::set<std::string> removed(removed_masters.begin(), removed_masters.end());
    for (const auto& [master, layouts] : layouts_of) {
        for (const auto& layout : layouts) {
            if (!used.contains(layout)) {
                removed.insert(layout);
            }
        }
    }
    if (removed.empty()) {
        return {};
    }

    PassResult result;

    // Body edits: the surviving masters' layout lists, the presentation's master list.
    for (const auto& master : masters) {
        if (removed.contains(master)) {
            continue;
        }
        const auto& layouts = layouts_of[master];
        if (std::none_of(layouts.begin(), layouts.end(), [&](const std::string& l) {
                return removed.contains(l);
            })) {
            continue;
        }
        IdList ids;
        if (!read_id_list(pkg, master, "sldLayoutIdLst", ids)) {
            return {};
        }
        if (drop_entries(ids, removed)) {
            result.changes.replaced[master] = save_part(ids.doc);
        }
    }
    if (!removed_masters.empty() && drop_entries(pres, removed)) {
        result.changes.replaced[pres_part] = save_part(pres.doc);
    }

    // Whatever was reachable only through the removed layouts and masters.
    const auto alive = reachable_without(pkg, removed);
    for (const auto& part : graph.reachable()) {
        if (!alive.contains(part)) {
            removed.insert(part);
        }
    }

    for (const auto& part : removed) {
        if (const auto size = pkg.part_size(part)) {
            result.changes.removed.insert(part);
            result.actions.push_back({part, "removed", detail_for(part), *size, 0});
        }
    }
    std::stable_sort(result.actions.begin(),
                     result.actions.end(),
                     [](const OptimizeAction& a, const OptimizeAction& b) {
                         return detail_rank(a.detail) < detail_rank(b.detail);
                     });
    return result;
}

}  // namespace pptctl::detail
