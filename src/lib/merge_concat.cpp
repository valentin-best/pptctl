// merge_concat.cpp — the output's presentation-level parts (#136): slide
// list, masters, notes master, sections, slide size check, app.xml counts.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <utility>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kNotesSlideType =
    "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";
// The PowerPoint 2010 extension that holds p14:sectionLst.
constexpr std::string_view kSectionExtUri = "{521415D9-36F7-43E2-AB2F-B90AF26B5E84}";
constexpr std::string_view kP14Ns = "http://schemas.microsoft.com/office/powerpoint/2010/main";
constexpr std::string_view kModernCommentsRel =
    "http://schemas.microsoft.com/office/2018/10/relationships/comments";

// Slide ids live in [256, 2^31); master and layout ids share [2^31, 2^32).
constexpr std::uint32_t kFirstSlideId = 256;
constexpr std::uint32_t kFirstMasterId = 2'147'483'648U;

std::string rel_type(std::string_view name) {
    return std::string(kRelNs) + std::string(name);
}

// Parse keeping the declaration, so re-serializing preserves it.
bool load_xml(pugi::xml_document& doc, const std::vector<std::byte>& xml) {
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    return static_cast<bool>(doc.load_buffer(xml.data(), xml.size(), flags, pugi::encoding_utf8));
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

pugi::xml_node child_named(const pugi::xml_node& parent, std::string_view local) {
    return parent.find_child([&](const pugi::xml_node& n) { return local_name(n) == local; });
}

// The target of `source`'s first relationship of `type`, or "".
std::string target_of_type(const Package& pkg, std::string_view source, std::string_view type) {
    for (const auto& rel : pkg.relationships(source)) {
        if (!rel.external && rel.type == type) {
            return rel.target;
        }
    }
    return {};
}

std::string presentation_part(const Package& pkg) {
    return target_of_type(pkg, "", rel_type("officeDocument"));
}

std::uint32_t id_of(const pugi::xml_node& node) {
    return node.attribute("id").as_uint(0);
}

// A deterministic GUID ("{8-4-4-4-12}", uppercase hex) derived from `seed`.
std::string make_guid(std::string_view seed) {
    const std::string salted = std::string(seed) + "#pptctl";
    const auto hi = fnv1a64(std::as_bytes(std::span(seed.data(), seed.size())));
    const auto lo = fnv1a64(std::as_bytes(std::span(salted.data(), salted.size())));
    const auto hex = std::format("{:016X}{:016X}", hi, lo);
    return std::format("{{{}-{}-{}-{}-{}}}",
                       hex.substr(0, 8),
                       hex.substr(8, 4),
                       hex.substr(12, 4),
                       hex.substr(16, 4),
                       hex.substr(20, 12));
}

struct Section {
    std::string name;
    std::string guid;
    std::vector<std::uint32_t> slide_ids;
};

struct SourceSlide {
    std::uint32_t id = 0;
    std::string rid;
    std::string part;
};

// What concatenate needs from one input's presentation.xml.
struct DeckLayout {
    std::vector<SourceSlide> slides;
    std::vector<Section> sections;
    bool has_sections = false;
    std::int64_t cx = 0;
    std::int64_t cy = 0;
};

DeckLayout read_layout(const Package& pkg) {
    DeckLayout out;
    const auto pres_part = presentation_part(pkg);
    pugi::xml_document doc;
    if (!load_xml(doc, pkg.read_part(pres_part))) {
        return out;
    }
    const auto pres = doc.child("p:presentation");
    for (const auto& node : pres.child("p:sldIdLst").children("p:sldId")) {
        const std::string rid = node.attribute("r:id").as_string("");
        out.slides.push_back({id_of(node), rid, pkg.resolve(pres_part, rid)});
    }
    const auto size = pres.child("p:sldSz");
    out.cx = size.attribute("cx").as_llong(0);
    out.cy = size.attribute("cy").as_llong(0);
    for (const auto& ext : pres.child("p:extLst").children("p:ext")) {
        if (ext.attribute("uri").as_string("") != kSectionExtUri) {
            continue;
        }
        const auto list = child_named(ext, "sectionLst");
        out.has_sections = static_cast<bool>(list);
        for (const auto& node : list.children()) {
            if (local_name(node) != "section") {
                continue;
            }
            Section section{
                node.attribute("name").as_string(""), node.attribute("id").as_string(""), {}};
            for (const auto& sld : child_named(node, "sldIdLst").children()) {
                section.slide_ids.push_back(id_of(sld));
            }
            out.sections.push_back(std::move(section));
        }
    }
    return out;
}

// The `.rels` of the presentation part, gaining relationships with fresh ids.
class PresentationRels {
 public:
    explicit PresentationRels(const std::vector<std::byte>& xml) {
        load_xml(doc_, xml);
        for (const auto& node : doc_.child("Relationships").children("Relationship")) {
            const std::string_view id = node.attribute("Id").as_string("");
            if (id.starts_with("rId")) {
                next_ =
                    std::max<std::uint64_t>(next_, std::strtoull(id.data() + 3, nullptr, 10) + 1);
            }
        }
    }

    // Add a relationship from `pres_part` to `target` and return its id.
    std::string add(std::string_view type,
                    const std::string& pres_part,
                    const std::string& target) {
        const auto id = std::format("rId{}", next_++);
        auto node = doc_.child("Relationships").append_child("Relationship");
        node.append_attribute("Id") = id.c_str();
        node.append_attribute("Type") = std::string(type).c_str();
        node.append_attribute("Target") = relative_target(pres_part, target).c_str();
        return id;
    }

    [[nodiscard]] std::vector<std::byte> bytes() const {
        return save_xml(doc_);
    }

 private:
    pugi::xml_document doc_;
    std::uint64_t next_ = 1;
};

// Insert `name` as a child of `pres` after the last of `after` present, else first.
pugi::xml_node insert_after_any(pugi::xml_node pres,
                                const char* name,
                                std::initializer_list<const char*> after) {
    pugi::xml_node anchor;
    for (const auto* a : after) {
        if (auto n = pres.child(a)) {
            anchor = n;
        }
    }
    return anchor ? pres.insert_child_after(name, anchor) : pres.prepend_child(name);
}

// Modern comments anchor to their slide by its sldId (pc:sldMk@sldId): point
// the comments imported with `slide` at the id it has in the output.
void renumber_comment_anchors(MergeTarget& target,
                              const Package& source,
                              const std::map<std::string, std::string>& imported,
                              const SourceSlide& slide,
                              std::uint32_t new_id) {
    if (new_id == slide.id) {
        return;
    }
    for (const auto& rel : source.relationships(slide.part)) {
        const auto it = imported.find(rel.target);
        if (rel.external || rel.type != kModernCommentsRel || it == imported.end()) {
            continue;
        }
        pugi::xml_document doc;
        if (!load_xml(doc, target.read_part(it->second))) {
            continue;
        }
        bool changed = false;
        for (const auto& found : doc.select_nodes("//*[local-name()='sldMk']")) {
            auto id = found.node().attribute("sldId");
            if (id && id.as_uint() == slide.id) {
                id.set_value(new_id);
                changed = true;
            }
        }
        if (changed) {
            target.replace_part(it->second, save_xml(doc));
        }
    }
}

std::string inches(std::int64_t emu) {
    return std::format("{:.2f}", static_cast<double>(emu) / 914400.0);
}

}  // namespace

std::string add_presentation_relationship(MergeTarget& target,
                                          std::string_view type,
                                          const std::string& target_part) {
    const auto pres_part = presentation_part(target.base());
    const auto rels_part = rels_part_for(pres_part);
    PresentationRels rels(target.read_part(rels_part));
    auto rid = rels.add(type, pres_part, target_part);
    target.replace_part(rels_part, rels.bytes());
    return rid;
}

void concatenate(MergeTarget& target,
                 std::span<ConcatDeck> decks,
                 const std::string& imported_notes_master) {
    const auto& base = target.base();
    const auto pres_part = presentation_part(base);
    const auto rels_part = rels_part_for(pres_part);
    pugi::xml_document doc;
    if (!load_xml(doc, target.read_part(pres_part))) {
        return;
    }
    auto pres = doc.child("p:presentation");
    PresentationRels rels(target.read_part(rels_part));

    std::vector<DeckLayout> layouts;
    const DeckLayout* base_layout = nullptr;
    for (const auto& deck : decks) {
        layouts.push_back(read_layout(*deck.source));
    }
    for (std::size_t i = 0; i < decks.size(); ++i) {
        if (decks[i].imported == nullptr) {
            base_layout = &layouts[i];
        }
    }

    // --- slide list, in deck order ------------------------------------------
    std::uint32_t next_slide_id = kFirstSlideId;
    if (base_layout != nullptr) {
        for (const auto& s : base_layout->slides) {
            next_slide_id = std::max(next_slide_id, s.id + 1);
        }
    }
    auto list = pres.child("p:sldIdLst");
    if (!list) {
        list = insert_after_any(
            pres, "p:sldIdLst", {"p:sldMasterIdLst", "p:notesMasterIdLst", "p:handoutMasterIdLst"});
    }
    list.remove_children();
    // Per deck: source slide id -> output slide id, for the sections.
    std::vector<std::map<std::uint32_t, std::uint32_t>> slide_ids(decks.size());
    for (std::size_t i = 0; i < decks.size(); ++i) {
        auto& deck = decks[i];
        for (const auto& slide : layouts[i].slides) {
            std::uint32_t id = slide.id;
            std::string rid = slide.rid;
            if (deck.imported != nullptr) {
                const auto it = deck.imported->find(slide.part);
                if (it == deck.imported->end()) {
                    continue;  // not imported; the importer reported why
                }
                id = next_slide_id++;
                rid = rels.add(rel_type("slide"), pres_part, it->second);
                renumber_comment_anchors(target, *deck.source, *deck.imported, slide, id);
            }
            auto node = list.append_child("p:sldId");
            node.append_attribute("id") = id;
            node.append_attribute("r:id") = rid.c_str();
            slide_ids[i][slide.id] = id;
            ++deck.slides;
        }
    }

    // --- slide masters ------------------------------------------------------
    auto masters = pres.child("p:sldMasterIdLst");
    if (!masters) {
        masters = pres.prepend_child("p:sldMasterIdLst");
    }
    std::uint32_t next_master_id = kFirstMasterId;
    for (const auto& node : masters.children("p:sldMasterId")) {
        next_master_id = std::max(next_master_id, id_of(node) + 1);
        const auto master = base.resolve(pres_part, node.attribute("r:id").as_string(""));
        pugi::xml_document master_doc;
        if (!master.empty() && load_xml(master_doc, base.read_part(master))) {
            for (const auto& layout : master_doc.child("p:sldMaster")
                                          .child("p:sldLayoutIdLst")
                                          .children("p:sldLayoutId")) {
                next_master_id = std::max(next_master_id, id_of(layout) + 1);
            }
        }
    }
    for (const auto& deck : decks) {
        for (const auto& master : deck.masters) {
            auto node = masters.append_child("p:sldMasterId");
            node.append_attribute("id") = next_master_id++;
            node.append_attribute("r:id") =
                rels.add(rel_type("slideMaster"), pres_part, master).c_str();
            pugi::xml_document master_doc;
            if (!load_xml(master_doc, target.read_part(master))) {
                continue;
            }
            for (auto layout : master_doc.child("p:sldMaster")
                                   .child("p:sldLayoutIdLst")
                                   .children("p:sldLayoutId")) {
                layout.attribute("id").set_value(next_master_id++);
            }
            target.replace_part(master, save_xml(master_doc));
        }
    }

    // --- notes master -------------------------------------------------------
    // Listed in p:notesMasterIdLst: the imported one, or the base's when the
    // base only relates it (python-pptx output does), which Keynote refuses
    // to open.
    if (!pres.child("p:notesMasterIdLst")) {
        std::string rid;
        if (!imported_notes_master.empty()) {
            rid = rels.add(rel_type("notesMaster"), pres_part, imported_notes_master);
        } else {
            for (const auto& rel : base.relationships(pres_part)) {
                if (!rel.external && rel.type == rel_type("notesMaster")) {
                    rid = rel.id;
                    break;
                }
            }
        }
        if (!rid.empty()) {
            auto lst = pres.insert_child_after("p:notesMasterIdLst", masters);
            lst.append_child("p:notesMasterId").append_attribute("r:id") = rid.c_str();
        }
    }

    // --- sections -----------------------------------------------------------
    const bool any_sections = std::any_of(
        layouts.begin(), layouts.end(), [](const DeckLayout& l) { return l.has_sections; });
    if (any_sections) {
        auto ext_list = pres.child("p:extLst");
        if (!ext_list) {
            ext_list = pres.append_child("p:extLst");
        }
        auto ext =
            ext_list.find_child_by_attribute("p:ext", "uri", std::string(kSectionExtUri).c_str());
        if (!ext) {
            ext = ext_list.append_child("p:ext");
            ext.append_attribute("uri") = std::string(kSectionExtUri).c_str();
        }
        ext.remove_children();
        auto section_list = ext.append_child("p14:sectionLst");
        section_list.append_attribute("xmlns:p14") = std::string(kP14Ns).c_str();
        std::set<std::string> guids;
        const auto add_section = [&](const std::string& name,
                                     std::string guid,
                                     const std::vector<std::uint32_t>& ids,
                                     std::string_view seed) {
            for (int n = 0; guid.empty() || !guids.insert(guid).second; ++n) {
                guid = make_guid(std::format("{}/{}/{}", seed, name, n));
            }
            auto node = section_list.append_child("p14:section");
            node.append_attribute("name") = name.c_str();
            node.append_attribute("id") = guid.c_str();
            auto sld_ids = node.append_child("p14:sldIdLst");
            for (const auto id : ids) {
                sld_ids.append_child("p14:sldId").append_attribute("id") = id;
            }
        };
        for (std::size_t i = 0; i < decks.size(); ++i) {
            const auto seed = std::format("{}:{}", i, decks[i].path);
            if (layouts[i].has_sections) {
                for (const auto& section : layouts[i].sections) {
                    std::vector<std::uint32_t> ids;
                    for (const auto id : section.slide_ids) {
                        if (const auto it = slide_ids[i].find(id); it != slide_ids[i].end()) {
                            ids.push_back(it->second);
                        }
                    }
                    add_section(section.name, section.guid, ids, seed);
                }
            } else {
                std::vector<std::uint32_t> ids;
                for (const auto& slide : layouts[i].slides) {
                    if (const auto it = slide_ids[i].find(slide.id); it != slide_ids[i].end()) {
                        ids.push_back(it->second);
                    }
                }
                add_section(std::filesystem::path(decks[i].path).stem().string(), {}, ids, seed);
            }
        }
    }

    // --- slide size ---------------------------------------------------------
    if (base_layout != nullptr) {
        for (std::size_t i = 0; i < decks.size(); ++i) {
            const auto& l = layouts[i];
            if (decks[i].imported != nullptr && l.cx > 0 && l.cy > 0 &&
                (l.cx != base_layout->cx || l.cy != base_layout->cy)) {
                decks[i].warnings.push_back(std::format(
                    "slide size {} x {} in differs from the base deck's {} x {} in; its slides "
                    "keep their geometry, unscaled",
                    inches(l.cx),
                    inches(l.cy),
                    inches(base_layout->cx),
                    inches(base_layout->cy)));
            }
        }
    }

    target.replace_part(pres_part, save_xml(doc));
    target.replace_part(rels_part, rels.bytes());

    // --- docProps/app.xml counts --------------------------------------------
    const auto app_part = target_of_type(base, "", rel_type("extended-properties"));
    pugi::xml_document app;
    if (app_part.empty() || !load_xml(app, target.read_part(app_part))) {
        return;
    }
    std::size_t slides = 0;
    for (const auto& deck : decks) {
        slides += deck.slides;
    }
    const auto names = target.part_names();
    const auto notes = std::count_if(names.begin(), names.end(), [&](const std::string& n) {
        return target.content_type(n) == kNotesSlideType;
    });
    auto props = app.child("Properties");
    if (auto node = props.child("Slides")) {
        node.text().set(std::to_string(slides).c_str());
    }
    if (auto node = props.child("Notes")) {
        node.text().set(std::to_string(notes).c_str());
    }
    target.replace_part(app_part, save_xml(app));
}

}  // namespace pptctl::detail
