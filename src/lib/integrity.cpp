// integrity.cpp — the package-integrity rule registry.

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <pptctl/integrity.hpp>
#include <pugixml.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl {

namespace {

constexpr std::string_view kRelsDir = "_rels/";
constexpr std::string_view kRelsExt = ".rels";

// The relationships namespace, Transitional and Strict.
constexpr std::array<std::string_view, 2> kRelNamespaces = {
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
    "http://purl.oclc.org/ooxml/officeDocument/relationships"};

bool is_relationships_ns(std::string_view uri) {
    return std::find(kRelNamespaces.begin(), kRelNamespaces.end(), uri) != kRelNamespaces.end();
}

// The URI `prefix` is bound to at `el`: the nearest xmlns:prefix on it or an
// ancestor. pugixml doesn't track namespaces, so this resolves them by hand.
std::string_view namespace_of(pugi::xml_node el, std::string_view prefix) {
    const std::string decl = "xmlns:" + std::string(prefix);
    for (auto n = el; n; n = n.parent()) {
        if (const auto a = n.attribute(decl.c_str())) {
            return a.value();
        }
    }
    return {};
}

// Collects every relationship-namespace attribute with a non-empty value.
struct RefCollector : pugi::xml_tree_walker {
    std::vector<detail::RelationshipRef> refs;

    bool for_each(pugi::xml_node& node) override {
        if (node.type() != pugi::node_element) {
            return true;
        }
        for (const auto& a : node.attributes()) {
            const std::string_view name = a.name();
            const auto colon = name.find(':');
            if (colon == std::string_view::npos || name.starts_with("xmlns:") ||
                *a.value() == '\0') {
                continue;
            }
            if (is_relationships_ns(namespace_of(node, name.substr(0, colon)))) {
                const std::string_view element = node.name();
                const auto element_colon = element.find(':');
                refs.push_back({std::string(name),
                                a.value(),
                                std::string(element_colon == std::string_view::npos
                                                ? element
                                                : element.substr(element_colon + 1))});
            }
        }
        return true;
    }
};

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

std::vector<detail::RelationshipRef> detail::relationship_refs(std::span<const std::byte> xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8)) {
        return {};
    }
    RefCollector collector;
    doc.traverse(collector);
    return std::move(collector.refs);
}

const std::vector<RegisteredIntegrityRule>& integrity_rules() {
    static const std::vector<RegisteredIntegrityRule> rules = {
        {"dangling-rel", detail::integrity_dangling_rel},
        {"broken-rid", detail::integrity_broken_rid},
        {"undeclared-ctype", detail::integrity_undeclared_ctype},
        {"orphan-part", detail::integrity_orphan_part},
        {"shared-part", detail::integrity_shared_part},
        {"unlisted-part", detail::integrity_unlisted_part},
        {"notes-clrmap", detail::integrity_notes_clrmap},
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
