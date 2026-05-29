// pptctl/package.hpp
//
// Package layer: a .pptx is a ZIP containing XML "parts" and relationship
// files. This abstraction opens a package, lists its parts, and resolves
// relationships. It does NOT parse part contents — that's the document
// layer's job. The split is intentional: most subcommands only need
// metadata, not full XML parsing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

// A single OOXML relationship, as declared in a part's `_rels/*.rels` file.
struct Relationship {
    std::string id;         // relationship id, e.g. "rId1" (scoped to the source part)
    std::string type;       // relationship Type URI
    std::string target;     // resolved internal part name, or the raw URI when external
    bool external = false;  // true when TargetMode="External"
};

class Package {
 public:
    Package();
    ~Package();

    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;
    Package(Package&& other) noexcept;
    Package& operator=(Package&& other) noexcept;

    // Open a .pptx file. Returns false on error.
    [[nodiscard]] bool open(const std::string& path);

    // True if a package is currently open.
    [[nodiscard]] bool is_open() const noexcept;

    // Number of parts in the package (entries in the ZIP).
    [[nodiscard]] std::size_t part_count() const noexcept;

    // Names of all parts (e.g. "ppt/slides/slide1.xml").
    [[nodiscard]] std::vector<std::string> part_names() const;

    // Read a part's bytes. Returns empty vector if part doesn't exist.
    [[nodiscard]] std::vector<std::byte> read_part(std::string_view name) const;

    // Content type for a part, resolved from `[Content_Types].xml`: an explicit
    // Override (matched by part path) wins over a Default (matched by extension).
    // Returns empty string if the part's content type can't be determined.
    [[nodiscard]] std::string content_type(std::string_view part_name) const;

    // Relationships declared by a source part, parsed from its `_rels/*.rels`
    // file. Pass an empty source_part for the package root (`_rels/.rels`).
    // Internal targets are resolved to absolute part names; external targets
    // keep their raw URI and are flagged. Empty if the part has no .rels file.
    [[nodiscard]] std::vector<Relationship> relationships(std::string_view source_part) const;

    // Resolve a single relationship id within source_part to its target part
    // name. Returns empty if the id is unknown or the target is external.
    [[nodiscard]] std::string resolve(std::string_view source_part, std::string_view rel_id) const;

    // Uncompressed size of a part, read from the ZIP entry metadata without
    // decompressing it. Returns nullopt if the part doesn't exist or no
    // package is open.
    [[nodiscard]] std::optional<std::uint64_t> part_size(std::string_view name) const;

 private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pptctl
