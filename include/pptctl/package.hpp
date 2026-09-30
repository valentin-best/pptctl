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
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

// FNV-1a 64-bit hash of a byte range. The package layer's content identity
// for parts: two decks may store the same image under different part names
// (or different images under the same name), so anything that compares media
// across packages keys on this rather than on the name.
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;

// A single OOXML relationship, as declared in a part's `_rels/*.rels` file.
struct Relationship {
    std::string id;         // relationship id, e.g. "rId1" (scoped to the source part)
    std::string type;       // relationship Type URI
    std::string target;     // resolved internal part name, or the raw URI when external
    bool external = false;  // true when TargetMode="External"
};

// The `_rels/*.rels` part that declares relationships for `part`. An empty
// part denotes the package root ("_rels/.rels"). Removing a part should also
// remove this file when it exists.
[[nodiscard]] std::string rels_part_for(std::string_view part);

// Rewrite `[Content_Types].xml`: drop the Override for every part in
// `removed_parts`, then set one Override per `added_overrides` entry
// (part name -> content type), replacing an existing Override for that part.
// Defaults are untouched. Part names are given without a leading slash, as
// Package::part_names reports them. The result is meant to be passed back to
// Package::write as the replaced `[Content_Types].xml`.
[[nodiscard]] std::vector<std::byte> rewrite_content_types(
    std::span<const std::byte> xml,
    const std::set<std::string>& removed_parts,
    const std::map<std::string, std::string>& added_overrides);

// Rewrite a `.rels` part owned by `source_part`: drop every Relationship
// whose resolved internal target is in `removed_parts`. Surviving ids are
// left as they are; external relationships are never dropped.
[[nodiscard]] std::vector<std::byte> rewrite_rels(std::span<const std::byte> xml,
                                                  std::string_view source_part,
                                                  const std::set<std::string>& removed_parts);

// The edits a mutating command applies when writing a package. Everything not
// named here is copied through untouched. A `replaced` name that doesn't exist
// in the source is added as a new part. A name in both sets is removed.
struct PartChanges {
    std::map<std::string, std::vector<std::byte>> replaced;  // part name -> new bytes
    std::set<std::string> removed;                           // part names to drop
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

    // At most the first `max_bytes` bytes of a part, decompressing only that
    // much. For header probing (image dimensions) of parts that may be
    // megabytes. Empty if the part doesn't exist.
    [[nodiscard]] std::vector<std::byte> read_part_prefix(std::string_view name,
                                                          std::size_t max_bytes) const;

    // fnv1a64 of a part's bytes, computed on first request and cached for the
    // package's lifetime, so media shared by many slides is hashed once.
    // Returns nullopt if the part doesn't exist.
    [[nodiscard]] std::optional<std::uint64_t> part_hash(std::string_view name) const;

    // Write the package to `out_path`, applying `changes`. Untouched entries
    // are copied raw (compressed bytes, no inflate/deflate) in their original
    // order; replaced parts keep their position and entry metadata; new parts
    // are appended in name order. The output is written to a temp file next to
    // the target and renamed over it on success, so a failure never leaves a
    // half-written .pptx. The target's directory must already exist. A
    // symlinked target is followed: the file it points to is replaced.
    //
    // Writing over the package's own file is allowed: the reader is closed
    // before the rename and reopened on the new file, so the Package stays
    // usable and reflects what was written. Returns false on any error.
    [[nodiscard]] bool write(const std::string& out_path, const PartChanges& changes);

 private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pptctl
