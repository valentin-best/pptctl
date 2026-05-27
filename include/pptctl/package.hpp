// pptctl/package.hpp
//
// Package layer: a .pptx is a ZIP containing XML "parts" and relationship
// files. This abstraction opens a package, lists its parts, and resolves
// relationships. It does NOT parse part contents — that's the document
// layer's job. The split is intentional: most subcommands only need
// metadata, not full XML parsing.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

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

   private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pptctl
