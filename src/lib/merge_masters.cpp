// merge_masters.cpp — collapsing identical slide masters across merged decks (#139).

#include <format>
#include <string>
#include <utility>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kSlideMasterRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster";
constexpr std::string_view kOfficeDocumentRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";

// The slide masters `pkg`'s presentation part relates.
std::vector<std::string> masters_of(const Package& pkg) {
    std::string pres;
    for (const auto& rel : pkg.relationships("")) {
        if (!rel.external && rel.type == kOfficeDocumentRel) {
            pres = rel.target;
            break;
        }
    }
    std::vector<std::string> out;
    for (const auto& rel : pkg.relationships(pres)) {
        if (!rel.external && rel.type == kSlideMasterRel) {
            out.push_back(rel.target);
        }
    }
    return out;
}

}  // namespace

MasterClosure master_closure(const Package& pkg, const std::string& master) {
    MasterClosure out;
    std::map<std::string, std::size_t> index;  // part -> position in the walk
    const auto visit = [&](const std::string& part) {
        const auto [it, inserted] = index.emplace(part, out.parts.size());
        if (inserted) {
            out.parts.push_back(part);
        }
        return it->second;
    };
    visit(master);
    // One line per part, then one per relationship. A target inside the
    // closure is named by its position, so the signature is name-free.
    for (std::size_t k = 0; k < out.parts.size(); ++k) {
        const auto part = out.parts[k];  // a copy: visit() may reallocate
        out.signature += std::format("P{}:{:016x}:{}:{}\n",
                                     k,
                                     pkg.part_hash(part).value_or(0),
                                     pkg.part_size(part).value_or(0),
                                     pkg.content_type(part));
        for (const auto& rel : pkg.relationships(part)) {
            std::string target;
            if (rel.external) {
                target = "external:" + rel.target;
            } else if (!pkg.part_size(rel.target)) {
                target = "missing:" + rel.target;
            } else {
                target = std::format("#{}", visit(rel.target));
            }
            out.signature += std::format("  {}|{}|{}\n", rel.id, rel.type, target);
        }
    }
    return out;
}

MasterIndex::MasterIndex(const Package& base) {
    for (const auto& master : masters_of(base)) {
        auto closure = master_closure(base, master);
        auto output = closure.parts;
        entries_.push_back({&base, std::move(closure), std::move(output)});
    }
}

std::map<std::string, std::string> MasterIndex::match(const Package& source,
                                                      const std::string& master) const {
    const auto closure = master_closure(source, master);
    for (const auto& entry : entries_) {
        if (entry.closure.signature != closure.signature) {
            continue;
        }
        bool same = true;
        for (std::size_t k = 0; same && k < closure.parts.size(); ++k) {
            same =
                source.read_part(closure.parts[k]) == entry.pkg->read_part(entry.closure.parts[k]);
        }
        if (same) {
            std::map<std::string, std::string> out;
            for (std::size_t k = 0; k < closure.parts.size(); ++k) {
                out.emplace(closure.parts[k], entry.output[k]);
            }
            return out;
        }
    }
    return {};
}

void MasterIndex::add(const Package& source,
                      const std::string& master,
                      const std::map<std::string, std::string>& mapping) {
    auto closure = master_closure(source, master);
    std::vector<std::string> output;
    for (const auto& part : closure.parts) {
        const auto it = mapping.find(part);
        if (it == mapping.end()) {
            return;  // not wholly in the output: nothing can map onto it
        }
        output.push_back(it->second);
    }
    entries_.push_back({&source, std::move(closure), std::move(output)});
}

}  // namespace pptctl::detail
