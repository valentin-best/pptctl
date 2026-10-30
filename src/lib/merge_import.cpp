// merge_import.cpp — copying a part's closure from one package into another
// (#135): name allocation, relationship retargeting, content types.

#include <algorithm>
#include <cctype>
#include <format>
#include <pugixml.hpp>
#include <string>
#include <utility>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

constexpr std::string_view kContentTypes = "[Content_Types].xml";
constexpr std::string_view kRelsType = "application/vnd.openxmlformats-package.relationships+xml";

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Extension of a part name without the dot, lowercased; "" if none.
std::string extension_of(std::string_view name) {
    const auto dot = name.find_last_of('.');
    const auto slash = name.find_last_of('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return to_lower(name.substr(dot + 1));
}

bool all_digits(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

MergeTarget::MergeTarget(const Package& base) : base_(&base) {
    for (const auto& name : base.part_names()) {
        taken_.insert(to_lower(name));
    }
    const auto xml = base.read_part(kContentTypes);
    pugi::xml_document doc;
    if (doc.load_buffer(xml.data(), xml.size())) {
        for (const auto& node : doc.child("Types").children("Default")) {
            defaults_[to_lower(node.attribute("Extension").as_string(""))] =
                node.attribute("ContentType").as_string("");
        }
    }
}

std::string MergeTarget::allocate(std::string_view source_name) {
    const auto slash = source_name.find_last_of('/');
    const auto file_start = slash == std::string_view::npos ? 0 : slash + 1;
    auto dot = source_name.find_last_of('.');
    if (dot == std::string_view::npos || dot < file_start) {
        dot = source_name.size();
    }
    auto stem_end = dot;
    while (stem_end > file_start &&
           std::isdigit(static_cast<unsigned char>(source_name[stem_end - 1])) != 0) {
        --stem_end;
    }
    const auto prefix = source_name.substr(0, stem_end);  // directory + stem
    const auto ext = source_name.substr(dot);             // ".xml", or ""

    // Highest index in the family; an unnumbered member counts as 0.
    const auto lower_prefix = to_lower(prefix);
    const auto lower_ext = to_lower(ext);
    std::size_t highest = 0;
    for (auto it = taken_.lower_bound(lower_prefix);
         it != taken_.end() && it->starts_with(lower_prefix);
         ++it) {
        const std::string_view name = *it;
        if (!name.ends_with(lower_ext) || name.size() < lower_prefix.size() + lower_ext.size()) {
            continue;
        }
        const auto digits =
            name.substr(lower_prefix.size(), name.size() - lower_prefix.size() - lower_ext.size());
        if (digits.size() <= 9 && all_digits(digits)) {
            highest = std::max<std::size_t>(highest,
                                            digits.empty() ? 0 : std::stoul(std::string(digits)));
        }
    }
    auto name = std::format("{}{}{}", prefix, highest + 1, ext);
    taken_.insert(to_lower(name));
    return name;
}

void MergeTarget::add_part(const std::string& name,
                           std::vector<std::byte> bytes,
                           const std::string& content_type) {
    changes_.replaced[name] = std::move(bytes);
    added_types_[name] = content_type;
    added_.push_back(name);
    if (content_type.empty()) {
        return;
    }
    const auto ext = extension_of(name);
    if (ext.empty()) {
        added_overrides_[name] = content_type;
        return;
    }
    const auto it = defaults_.find(ext);
    if (it == defaults_.end()) {
        defaults_[ext] = content_type;
        added_defaults_[ext] = content_type;
    } else if (it->second != content_type) {
        added_overrides_[name] = content_type;
    }
}

void MergeTarget::replace_part(const std::string& name, std::vector<std::byte> bytes) {
    changes_.replaced[name] = std::move(bytes);
}

std::vector<std::byte> MergeTarget::read_part(const std::string& name) const {
    const auto it = changes_.replaced.find(name);
    return it != changes_.replaced.end() ? it->second : base_->read_part(name);
}

std::string MergeTarget::content_type(const std::string& name) const {
    const auto it = added_types_.find(name);
    return it != added_types_.end() ? it->second : base_->content_type(name);
}

std::vector<std::string> MergeTarget::part_names() const {
    auto names = base_->part_names();
    for (const auto& [name, type] : added_types_) {
        names.push_back(name);
    }
    return names;
}

PartChanges MergeTarget::changes() const {
    auto out = changes_;
    if (!added_defaults_.empty() || !added_overrides_.empty()) {
        out.replaced[std::string(kContentTypes)] = rewrite_content_types(
            base_->read_part(kContentTypes), {}, added_overrides_, added_defaults_);
    }
    return out;
}

PartImporter::PartImporter(const Package& source, MergeTarget& target, PartResolver resolve)
    : source_(&source), target_(&target), resolve_(std::move(resolve)) {}

std::string PartImporter::import(const std::string& part) {
    if (const auto it = mapping_.find(part); it != mapping_.end()) {
        return it->second;
    }

    // Walk the closure first, naming each part as it is found, so every
    // relationship can be re-pointed once all of its targets have names.
    std::vector<std::string> pending;
    const auto reach = [&](const std::string& p, std::string_view rel_type) {
        if (mapping_.contains(p)) {
            return true;
        }
        if (resolve_) {
            if (auto existing = resolve_(p, rel_type)) {
                mapping_[p] = std::move(*existing);
                return true;
            }
        }
        if (!source_->part_size(p)) {
            return false;
        }
        mapping_[p] = target_->allocate(p);
        pending.push_back(p);
        return true;
    };
    if (!reach(part, {})) {
        return {};
    }
    // `pending` grows as the walk finds parts, so no iterators into it.
    std::size_t walked = 0;
    while (walked < pending.size()) {
        const auto from = pending[walked++];  // a copy: reach() may reallocate
        for (const auto& rel : source_->relationships(from)) {
            if (!rel.external && !reach(rel.target, rel.type)) {
                warnings_.push_back(std::format(
                    "{}: relationship {} targets {}, which is missing", from, rel.id, rel.target));
            }
        }
    }

    for (const auto& from : pending) {
        const auto& to = mapping_.at(from);
        const auto content_type = source_->content_type(from);
        if (content_type.empty()) {
            warnings_.push_back(std::format("{}: no content type declared", from));
        }
        target_->add_part(to, source_->read_part(from), content_type);
        const auto rels = rels_part_for(from);
        if (source_->part_size(rels)) {
            target_->add_part(rels_part_for(to),
                              retarget_rels(source_->read_part(rels), from, to, mapping_),
                              std::string(kRelsType));
        }
        copied_.push_back(from);
    }
    return mapping_.at(part);
}

}  // namespace pptctl::detail
