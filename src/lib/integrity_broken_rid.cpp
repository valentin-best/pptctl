// integrity_broken_rid.cpp — the "broken-rid" rule: r:embed / r:id / r:link
// not defined in the part's .rels (#130).

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <map>
#include <pptctl/presentation.hpp>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

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
    struct Ref {
        std::string qname;
        std::string id;
    };
    std::vector<Ref> refs;

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
                refs.push_back({std::string(name), a.value()});
            }
        }
        return true;
    }
};

bool is_xml_content_type(std::string_view ct) {
    return ct.ends_with("+xml") || ct.ends_with("/xml");
}

// Slide part -> 1-based slide number, numbered the way lint numbers slides.
// Empty when presentation.xml doesn't load: findings then carry slide 0.
std::map<std::string, std::size_t, std::less<>> slide_numbers(const Package& pkg) {
    std::map<std::string, std::size_t, std::less<>> out;
    Presentation pres(pkg);
    if (pres.load()) {
        for (std::size_t i = 0; i < pres.slide_parts().size(); ++i) {
            out.emplace(pres.slide_parts()[i], i + 1);
        }
    }
    return out;
}

}  // namespace

// Every reachable XML part under ppt/, one parsed at a time. Unreachable parts
// are left to orphan-part; parts that don't parse are skipped.
void integrity_broken_rid(const Package& pkg,
                          const PartGraph& graph,
                          const LintOptions& /*opts*/,
                          std::vector<Diagnostic>& out) {
    const auto slides = slide_numbers(pkg);
    for (const auto& part : graph.reachable()) {
        if (!part.starts_with("ppt/") || !is_xml_content_type(pkg.content_type(part))) {
            continue;
        }
        const auto bytes = pkg.read_part(part);
        pugi::xml_document doc;
        if (!doc.load_buffer(
                bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
            continue;
        }
        RefCollector collector;
        doc.traverse(collector);
        if (collector.refs.empty()) {
            continue;
        }
        std::set<std::string, std::less<>> defined;
        for (const auto& rel : pkg.relationships(part)) {
            defined.insert(rel.id);
        }
        const auto slide = slides.find(part);
        for (const auto& ref : collector.refs) {
            if (defined.contains(ref.id)) {
                continue;
            }
            out.push_back(
                {"broken-rid",
                 Severity::Error,
                 slide == slides.end() ? 0 : slide->second,
                 "",
                 std::format(
                     R"({}="{}" is not defined in {})", ref.qname, ref.id, rels_part_for(part)),
                 part});
        }
    }
}

}  // namespace pptctl::detail
