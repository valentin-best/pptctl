// merge_comments.cpp — carrying comment authors over to the merged deck (#159).

#include <algorithm>
#include <cstdint>
#include <format>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <utility>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kLegacyCommentsType =
    "application/vnd.openxmlformats-officedocument.presentationml.comments+xml";
constexpr std::string_view kLegacyAuthorsType =
    "application/vnd.openxmlformats-officedocument.presentationml.commentAuthors+xml";
constexpr std::string_view kModernCommentsType = "application/vnd.ms-powerpoint.comments+xml";
constexpr std::string_view kModernAuthorsType = "application/vnd.ms-powerpoint.authors+xml";
constexpr std::string_view kModernAuthorsRel =
    "http://schemas.microsoft.com/office/2018/10/relationships/authors";
constexpr std::string_view kPNs = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view kP188Ns = "http://schemas.microsoft.com/office/powerpoint/2018/8/main";

// Parse keeping the declaration, so re-serializing preserves it.
bool load_xml(pugi::xml_document& doc, const std::vector<std::byte>& xml) {
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    return !xml.empty() &&
           static_cast<bool>(doc.load_buffer(xml.data(), xml.size(), flags, pugi::encoding_utf8));
}

// Serialize without indentation, the way Office writes parts.
std::vector<std::byte> save_xml(const pugi::xml_document& doc) {
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

std::string_view local_name(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

// `node`'s element children with local name `local`.
std::vector<pugi::xml_node> children_named(const pugi::xml_node& node, std::string_view local) {
    std::vector<pugi::xml_node> out;
    for (const auto& child : node.children()) {
        if (child.type() == pugi::node_element && local_name(child) == local) {
            out.push_back(child);
        }
    }
    return out;
}

// The part `pkg`'s presentation part relates with `type`, or "".
std::string presentation_target(const Package& pkg, std::string_view type) {
    std::string pres;
    for (const auto& rel : pkg.relationships("")) {
        if (!rel.external && rel.type == std::string(kRelNs) + "officeDocument") {
            pres = rel.target;
            break;
        }
    }
    for (const auto& rel : pkg.relationships(pres)) {
        if (!rel.external && rel.type == type) {
            return rel.target;
        }
    }
    return {};
}

// An author list part of the output: the base's, or a new one created on
// demand at `default_name`.
struct AuthorList {
    std::string part;  // the base's part, or "" until finish() names a new one
    pugi::xml_document doc;
    pugi::xml_node root;
    bool changed = false;

    AuthorList(const MergeTarget& target,
               std::string_view rel_type,
               const char* root_name,
               std::string_view ns) {
        part = presentation_target(target.base(), rel_type);
        if (!part.empty() && load_xml(doc, target.read_part(part))) {
            root = doc.document_element();
        }
        if (!root) {
            part.clear();
            auto decl = doc.prepend_child(pugi::node_declaration);
            decl.append_attribute("version") = "1.0";
            decl.append_attribute("encoding") = "UTF-8";
            decl.append_attribute("standalone") = "yes";
            root = doc.append_child(root_name);
            const std::string prefix(
                std::string_view(root_name).substr(0, std::string_view(root_name).find(':')));
            root.append_attribute(("xmlns:" + prefix).c_str()) = std::string(ns).c_str();
        }
    }

    // Write the list back if it changed: replace the base's part, or add a new
    // one at `default_name` with its relationship and content type.
    void finish(MergeTarget& target,
                const std::string& default_name,
                std::string_view rel_type,
                std::string_view content_type) const {
        if (!changed) {
            return;
        }
        if (!part.empty()) {
            target.replace_part(part, save_xml(doc));
            return;
        }
        const auto name =
            target.read_part(default_name).empty() ? default_name : target.allocate(default_name);
        target.add_part(name, save_xml(doc), std::string(content_type));
        (void)add_presentation_relationship(target, rel_type, name);
    }
};

}  // namespace

struct CommentAuthors::Legacy {
    AuthorList list;
    explicit Legacy(const MergeTarget& target)
        : list(target, std::string(kRelNs) + "commentAuthors", "p:cmAuthorLst", kPNs) {}

    // The output author id for `name` + `initials`, adding the author if new.
    std::uint32_t author(const std::string& name, const std::string& initials) {
        std::uint32_t next_id = 0;
        std::uint32_t next_color = 0;
        for (const auto& a : children_named(list.root, "cmAuthor")) {
            if (a.attribute("name").as_string() == name &&
                a.attribute("initials").as_string() == initials) {
                return a.attribute("id").as_uint();
            }
            next_id = std::max(next_id, a.attribute("id").as_uint() + 1);
            next_color = std::max(next_color, a.attribute("clrIdx").as_uint() + 1);
        }
        const std::string qname = std::string(list.root.name())
                                      .substr(0, std::string_view(list.root.name()).find(':') + 1) +
                                  "cmAuthor";
        auto a = list.root.append_child(qname.c_str());
        a.append_attribute("id") = next_id;
        a.append_attribute("name") = name.c_str();
        a.append_attribute("initials") = initials.c_str();
        a.append_attribute("lastIdx") = 0;
        a.append_attribute("clrIdx") = next_color;
        list.changed = true;
        return next_id;
    }

    // The next comment idx for output author `id`, recorded as its lastIdx.
    std::uint32_t next_idx(std::uint32_t id) {
        for (auto a : children_named(list.root, "cmAuthor")) {
            if (a.attribute("id").as_uint() == id) {
                const auto idx = a.attribute("lastIdx").as_uint() + 1;
                a.attribute("lastIdx").set_value(idx);
                list.changed = true;
                return idx;
            }
        }
        return 1;
    }
};

struct CommentAuthors::Modern {
    AuthorList list;
    explicit Modern(const MergeTarget& target)
        : list(target, kModernAuthorsRel, "p188:authorLst", kP188Ns) {}
};

CommentAuthors::CommentAuthors(MergeTarget& target)
    : target_(&target),
      legacy_(std::make_shared<Legacy>(target)),
      modern_(std::make_shared<Modern>(target)) {}

void CommentAuthors::import(const Package& source,
                            const std::vector<std::string>& comment_parts,
                            std::vector<std::string>& warnings) {
    std::vector<std::string> legacy;
    bool modern = false;
    for (const auto& part : comment_parts) {
        const auto type = target_->content_type(part);
        if (type == kLegacyCommentsType) {
            legacy.push_back(part);
        } else if (type == kModernCommentsType) {
            modern = true;
        }
    }

    if (!legacy.empty()) {
        // The source's authors: id -> (name, initials).
        std::map<std::uint32_t, std::pair<std::string, std::string>> authors;
        pugi::xml_document src;
        if (load_xml(src,
                     source.read_part(
                         presentation_target(source, std::string(kRelNs) + "commentAuthors")))) {
            for (const auto& a : children_named(src.document_element(), "cmAuthor")) {
                authors[a.attribute("id").as_uint()] = {a.attribute("name").as_string(),
                                                        a.attribute("initials").as_string()};
            }
        }
        // Renumber every comment, then follow reply links: (source author,
        // source idx) -> (output author, output idx) across the deck.
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>>
            moved;
        std::set<std::uint32_t> unknown;
        std::vector<std::pair<std::string, pugi::xml_document>> docs;
        for (const auto& part : legacy) {
            auto& [name, doc] = docs.emplace_back(part, pugi::xml_document{});
            if (!load_xml(doc, target_->read_part(name))) {
                continue;
            }
            for (auto cm : children_named(doc.document_element(), "cm")) {
                const auto author = cm.attribute("authorId").as_uint();
                const auto it = authors.find(author);
                if (it == authors.end() && unknown.insert(author).second) {
                    warnings.push_back(std::format(
                        "comment author {} isn't in the deck's commentAuthors.xml; its comments "
                        "are attributed to \"Unknown author\"",
                        author));
                }
                const auto out = it != authors.end()
                                     ? legacy_->author(it->second.first, it->second.second)
                                     : legacy_->author("Unknown author", "?");
                const auto idx = legacy_->next_idx(out);
                moved[{author, cm.attribute("idx").as_uint()}] = {out, idx};
                cm.attribute("authorId").set_value(out);
                cm.attribute("idx").set_value(idx);
            }
        }
        for (auto& [name, doc] : docs) {
            for (const auto& found : doc.select_nodes("//*[local-name()='parentCm']")) {
                auto parent = found.node();
                const auto it = moved.find(
                    {parent.attribute("authorId").as_uint(), parent.attribute("idx").as_uint()});
                if (it != moved.end()) {
                    parent.attribute("authorId").set_value(it->second.first);
                    parent.attribute("idx").set_value(it->second.second);
                }
            }
            target_->replace_part(name, save_xml(doc));
        }
    }

    if (modern) {
        pugi::xml_document src;
        if (load_xml(src, source.read_part(presentation_target(source, kModernAuthorsRel)))) {
            std::set<std::string> known;
            for (const auto& a : children_named(modern_->list.root, "author")) {
                known.insert(a.attribute("id").as_string());
            }
            for (const auto& a : children_named(src.document_element(), "author")) {
                if (known.insert(a.attribute("id").as_string()).second) {
                    modern_->list.root.append_copy(a);
                    modern_->list.changed = true;
                }
            }
        } else {
            warnings.emplace_back(
                "the deck's modern comments have no authors.xml; their authors stay unknown");
        }
    }
}

void CommentAuthors::finish() {
    legacy_->list.finish(*target_,
                         "ppt/commentAuthors.xml",
                         std::string(kRelNs) + "commentAuthors",
                         kLegacyAuthorsType);
    modern_->list.finish(*target_, "ppt/authors.xml", kModernAuthorsRel, kModernAuthorsType);
}

}  // namespace pptctl::detail
