// merge_internal.hpp — the merge pipeline's building blocks, shared between
// src/lib/merge*.cpp and the tests. Not part of the public API.

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <pptctl/package.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::detail {

// The output package being assembled: the base package plus the parts added to
// it so far. Owns the PartChanges handed to Package::write.
class MergeTarget {
 public:
    explicit MergeTarget(const Package& base);

    // A part name no base or added part has, in `source_name`'s family: same
    // directory, stem (trailing digits dropped) and extension, numbered one
    // past the highest index already used. "ppt/slides/slide1.xml" becomes
    // "ppt/slides/slide4.xml" when slides 1-3 exist. Names compare
    // case-insensitively, as OPC part names do. The name is reserved.
    [[nodiscard]] std::string allocate(std::string_view source_name);

    // Add a part (its name from allocate) with its content type, which is
    // declared through a Default when the extension is new to the package,
    // and through an Override when the Default for it says something else.
    // An empty content type declares nothing.
    void add_part(const std::string& name,
                  std::vector<std::byte> bytes,
                  const std::string& content_type);

    // The changes to write: the added parts, plus `[Content_Types].xml`
    // rewritten when anything needed declaring.
    [[nodiscard]] PartChanges changes() const;

    [[nodiscard]] const Package& base() const noexcept {
        return *base_;
    }

 private:
    const Package* base_;
    std::set<std::string> taken_;                  // lowercased base + allocated names
    std::map<std::string, std::string> defaults_;  // extension (lowercase) -> content type
    std::map<std::string, std::string> added_defaults_;
    std::map<std::string, std::string> added_overrides_;
    PartChanges changes_;
};

// Consulted before a part is copied: the name of an existing output part that
// stands in for it (a base layout under --theme, an identical media part), or
// nullopt to copy it. A part resolved this way is not copied and its own
// relationships are not followed.
using PartResolver = std::function<std::optional<std::string>(std::string_view source_part)>;

// Copies parts from one source package into a MergeTarget. Remembers where
// every source part went, so a part reached from several slides (a layout,
// a master, a slide another slide links to) is copied once.
class PartImporter {
 public:
    PartImporter(const Package& source, MergeTarget& target, PartResolver resolve = {});

    // Copy `part` and every part reachable from it through internal
    // relationships, each with its `.rels` re-pointed at the copies. Part
    // bodies are copied byte for byte: relationship ids are part-scoped and
    // kept. Returns the part's name in the output, or "" if `part` is missing
    // from the source.
    std::string import(const std::string& part);

    // Source part name -> output part name, for everything imported or resolved.
    [[nodiscard]] const std::map<std::string, std::string>& mapping() const noexcept {
        return mapping_;
    }
    // The source parts copied (not resolved), in copy order; `.rels` not included.
    [[nodiscard]] const std::vector<std::string>& copied() const noexcept {
        return copied_;
    }
    // Problems found on the way (a relationship to a missing part, a part
    // with no content type), for the merge report.
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept {
        return warnings_;
    }

 private:
    const Package* source_;
    MergeTarget* target_;
    PartResolver resolve_;
    std::map<std::string, std::string> mapping_;
    std::vector<std::string> copied_;
    std::vector<std::string> warnings_;
};

}  // namespace pptctl::detail
