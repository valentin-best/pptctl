// pptctl/image.hpp
//
// Header-only probing of raster image metadata. Reads intrinsic pixel
// dimensions from the first bytes of an image without decoding it, so a
// multi-megabyte photo costs a few dozen byte reads. Lives in the library
// layer because lint (and later optimize) both need it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pptctl {

struct ImageDimensions {
    std::uint32_t width = 0;   // pixels
    std::uint32_t height = 0;  // pixels
};

// Intrinsic pixel dimensions of a PNG or JPEG, read from its header. Returns
// nullopt for any other format, for truncated or malformed input, and for a
// declared width or height of zero. Never throws.
[[nodiscard]] std::optional<ImageDimensions> probe_image_dimensions(
    std::span<const std::byte> bytes) noexcept;

}  // namespace pptctl
