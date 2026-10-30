// merge_media.cpp — media de-duplication by content across merged decks (#137).

#include <algorithm>
#include <cctype>
#include <pptctl/integrity.hpp>
#include <string>

#include "merge_internal.hpp"

namespace pptctl::detail {

namespace {

// True for a part under `ppt/media/`, compared case-insensitively.
bool is_media(std::string_view part) {
    constexpr std::string_view kDir = "ppt/media/";
    return part.size() > kDir.size() &&
           std::equal(kDir.begin(), kDir.end(), part.begin(), [](char a, char b) {
               return a == std::tolower(static_cast<unsigned char>(b));
           });
}

}  // namespace

MediaIndex::MediaIndex(const MergeTarget& target) : target_(&target) {
    const auto& base = target.base();
    for (const auto& name : base.part_names()) {
        if (is_media(name)) {
            add(name, base.part_size(name).value_or(0));
        }
    }
    catch_up();  // anything the target gained before the index existed
}

void MediaIndex::add(const std::string& name, std::uint64_t size) {
    by_size_[size].push_back(name);
}

void MediaIndex::catch_up() {
    const auto& added = target_->added();
    for (; seen_ < added.size(); ++seen_) {
        const auto& name = added[seen_];
        if (is_media(name)) {
            add(name, target_->read_part(name).size());
        }
    }
}

std::optional<std::string> MediaIndex::find(const Package& source,
                                            std::string_view part,
                                            std::string_view rel_type) {
    if (!is_media(part) || is_exclusively_owned(rel_type)) {
        return std::nullopt;
    }
    const auto size = source.part_size(part);
    if (!size) {
        return std::nullopt;
    }
    catch_up();
    const auto candidates = by_size_.find(*size);
    if (candidates == by_size_.end()) {
        return std::nullopt;
    }
    const auto hash = source.part_hash(part);
    std::vector<std::byte> bytes;  // read once, on the first hash match
    for (const auto& name : candidates->second) {
        auto known = hashes_.find(name);
        if (known == hashes_.end()) {
            // Base media are never replaced, and added names never collide with
            // base ones: a base name hashes through the package's cache.
            const auto& base = target_->base();
            const auto h = base.part_size(name) ? base.part_hash(name)
                                                : std::optional(fnv1a64(target_->read_part(name)));
            known = hashes_.emplace(name, h.value_or(0)).first;
        }
        if (known->second != hash) {
            continue;
        }
        if (bytes.empty()) {
            bytes = source.read_part(part);
        }
        if (target_->read_part(name) == bytes) {
            return name;
        }
    }
    return std::nullopt;
}

}  // namespace pptctl::detail
