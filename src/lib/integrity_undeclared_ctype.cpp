// integrity_undeclared_ctype.cpp — the "undeclared-ctype" rule: parts missing
// from [Content_Types].xml, and Overrides for parts that don't exist (#131).

#include <algorithm>
#include <cctype>
#include <format>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "integrity_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kContentTypes = "[Content_Types].xml";

// OPC part names and extensions compare ASCII case-insensitively.
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Extension of the last path segment, lowercased; empty when there is none.
std::string extension_of(std::string_view part) {
    const auto slash = part.rfind('/');
    const std::string_view file = slash == std::string_view::npos ? part : part.substr(slash + 1);
    const auto dot = file.rfind('.');
    return dot == std::string_view::npos ? std::string{} : lower(file.substr(dot + 1));
}

}  // namespace

// Parses [Content_Types].xml itself rather than asking Package::content_type,
// because declarations match case-insensitively here and stale Overrides need
// the full list. A missing or unparsable file is one finding, not one per part.
void integrity_undeclared_ctype(const Package& pkg,
                                const PartGraph& /*graph*/,
                                const LintOptions& /*opts*/,
                                std::vector<Diagnostic>& out) {
    const auto bytes = pkg.read_part(kContentTypes);
    pugi::xml_document doc;
    const auto types =
        bytes.empty() || !doc.load_buffer(
                             bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)
            ? pugi::xml_node{}
            : doc.child("Types");
    if (!types) {
        out.push_back({"undeclared-ctype",
                       Severity::Error,
                       0,
                       "",
                       "[Content_Types].xml is missing or not XML",
                       std::string(kContentTypes)});
        return;
    }

    std::set<std::string, std::less<>> defaults;
    std::set<std::string, std::less<>> overridden;  // lowercased, without the leading '/'
    std::vector<std::string> override_names;        // as written, in document order
    for (const auto& node : types.children("Default")) {
        defaults.insert(lower(node.attribute("Extension").as_string("")));
    }
    for (const auto& node : types.children("Override")) {
        std::string name = node.attribute("PartName").as_string("");
        overridden.insert(lower(std::string_view(name).substr(name.starts_with('/') ? 1 : 0)));
        override_names.push_back(std::move(name));
    }

    const auto names = pkg.part_names();
    std::set<std::string, std::less<>> parts;  // lowercased, for stale Overrides
    std::vector<std::string> sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end());
    for (const auto& part : sorted) {
        if (part == kContentTypes || part.ends_with('/')) {  // directory entries aren't parts
            continue;
        }
        const std::string key = lower(part);
        parts.insert(key);
        const std::string ext = extension_of(part);
        if (overridden.contains(key) || (!ext.empty() && defaults.contains(ext))) {
            continue;
        }
        out.push_back(
            {"undeclared-ctype",
             Severity::Error,
             0,
             "",
             ext.empty()
                 ? std::format(R"(part "{}" has no content type: no extension and no Override)",
                               part)
                 : std::format(
                       R"(part "{}" has no content type: no Default for extension "{}" and no Override)",
                       part,
                       ext),
             part});
    }

    for (const auto& name : override_names) {
        if (!parts.contains(lower(std::string_view(name).substr(name.starts_with('/') ? 1 : 0)))) {
            out.push_back(
                {"undeclared-ctype",
                 Severity::Warning,
                 0,
                 "",
                 std::format(R"(Override for "{}" names a part that does not exist)", name),
                 std::string(kContentTypes)});
        }
    }
}

}  // namespace pptctl::detail
