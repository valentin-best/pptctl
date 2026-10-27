// Image downsampling for the optimize images pass (#103). Private to the
// library; not installed.

#pragma once

#include <cstdint>
#include <pptctl/image.hpp>

namespace pptctl::detail {

// Shrink `src` to `width` x `height` (each at most the source's, at least 1)
// by area averaging: every output pixel is the coverage-weighted mean of the
// source pixels under it, which is exact for any ratio and alias-free for the
// large ratios optimize meets (3-10x). Colour is averaged premultiplied by
// alpha, so fully transparent pixels don't bleed their (invisible) colour into
// the edges of what stays visible. Averaging happens on the stored (gamma
// encoded) values, as most image tools do. The result keeps src's channel
// layout and metadata (ICC profile, EXIF orientation, PNG colour chunks).
[[nodiscard]] Image downsample_area(const Image& src, std::uint32_t width, std::uint32_t height);

}  // namespace pptctl::detail
