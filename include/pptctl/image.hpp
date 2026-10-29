// pptctl/image.hpp
//
// Raster images in the library layer: lint and optimize both need them.
//
// Probing reads intrinsic pixel dimensions from the first bytes of an image
// without decoding it, so a multi-megabyte photo costs a few dozen byte reads.
//
// The codec decodes PNG and JPEG to 8-bit pixels and encodes them back (libpng
// and libjpeg-turbo, src/lib/image_codec.cpp). Decoding is strict about what it
// accepts: anything a re-encode could not reproduce faithfully (CMYK JPEGs,
// 16-bit or animated PNGs, ...) is refused with a reason rather than converted,
// so a caller that only re-encodes what decodes never alters an image's look.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg };

// The format named by the leading signature bytes. Never throws.
[[nodiscard]] ImageFormat sniff_image_format(std::span<const std::byte> bytes) noexcept;

// ── codec ─────────────────────────────────────────────────────────────────────

// A decoded raster: 8 bits per channel, rows top to bottom, no row padding,
// plus the metadata a re-encode must carry over for the image to look the
// same. Encoders write the metadata their format supports and ignore the rest.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t channels = 0;         // 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA
    std::vector<std::uint8_t> pixels;  // width * height * channels bytes

    // ICC colour profile (JPEG APP2 chunks / PNG iCCP); empty when none.
    std::vector<std::byte> icc_profile;
    // JPEG EXIF Orientation tag, 1-8; 0 when the JPEG has none. Decoding does
    // NOT apply it: pixels stay in stored order and encode_jpeg writes the tag
    // back (in an otherwise empty EXIF block), so renderers orient the result
    // exactly as they oriented the original.
    std::uint16_t exif_orientation = 0;
    // PNG colour chunks, carried through a PNG re-encode.
    std::optional<int> png_srgb_intent;                   // sRGB rendering intent
    std::optional<std::int32_t> png_gamma;                // gAMA, x100000
    std::optional<std::array<std::int32_t, 8>> png_chrm;  // cHRM white/r/g/b x,y, x100000

    [[nodiscard]] bool has_alpha() const noexcept {
        return channels == 2 || channels == 4;
    }
};

// What a decode produced: the image, or why there is none.
struct DecodeResult {
    std::optional<Image> image;
    std::string reason;  // set when image is empty, e.g. "CMYK JPEG", "16-bit PNG"
};

// Decoders refuse images above this many pixels rather than allocate for them.
inline constexpr std::uint64_t kMaxDecodePixels = std::uint64_t{1} << 27;  // ~134 MP

// Decode an 8-bit JPEG (baseline, progressive or arithmetic-coded) to gray (1
// channel) or RGB (3 channels). Refuses CMYK/YCCK and 12/16-bit JPEGs, and
// fails on corrupt or truncated data. Reads the ICC profile and EXIF
// orientation into the Image. Never throws.
[[nodiscard]] DecodeResult decode_jpeg(std::span<const std::byte> bytes) noexcept;

// Decode a PNG of 8 bits per channel or fewer, keeping its channel layout:
// gray, gray+alpha, RGB or RGBA. Palettes expand to RGB, or RGBA when they
// carry transparency; sub-8-bit gray expands to 8 bits; a tRNS colour key
// becomes an alpha channel. Refuses 16-bit and animated (APNG) images. Reads
// iCCP, sRGB, gAMA and cHRM into the Image. Never throws.
[[nodiscard]] DecodeResult decode_png(std::span<const std::byte> bytes) noexcept;

struct JpegEncodeOptions {
    int quality = 85;         // libjpeg quality scale, 1-100
    bool progressive = true;  // progressive scans (also implies optimised Huffman tables)
};

// Encode a 1- or 3-channel Image as a JFIF JPEG (4:2:0 for colour), with its
// ICC profile and, for orientations 2-8, its EXIF orientation. Returns empty
// bytes for an image with alpha or inconsistent dimensions. Never throws.
[[nodiscard]] std::vector<std::byte> encode_jpeg(const Image& image,
                                                 const JpegEncodeOptions& opts = {}) noexcept;

// Encode an Image as a PNG at maximum deflate compression, keeping its channel
// layout and colour chunks. An RGB(A) image with at most 256 distinct colours
// is written as a palette PNG instead (tRNS carries its alpha), which is
// lossless. Returns empty bytes for inconsistent dimensions. Never throws.
[[nodiscard]] std::vector<std::byte> encode_png(const Image& image) noexcept;

// A rectangle of pixels: top-left corner, then size.
struct PixelRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// What a lossless JPEG crop produced: the cropped JPEG, or why there is none.
struct JpegCropResult {
    std::vector<std::byte> bytes;        // empty on failure
    PixelRect kept;                      // the region actually kept, in source pixels
    std::uint16_t exif_orientation = 0;  // the source's, as decode_jpeg reads it
    std::string reason;                  // set when bytes is empty
};

// Crop a JPEG to `rect` without decoding it, the way `jpegtran -crop` does:
// the DCT coefficient blocks the rect covers are copied into a new JPEG, so
// the kept pixels decode exactly as before, with no generation loss. The rect
// is in stored pixel order (EXIF orientation is not applied). Its right and
// bottom edges are kept exactly; its left and top edges move out to the
// block grid (8 or 16 pixels, by chroma subsampling), so `kept` may start up
// to 15 pixels before `rect`. The output is progressive with optimised
// Huffman tables and keeps every APPn and COM marker (ICC profile, EXIF, ...)
// except the JFIF/Adobe headers the encoder writes itself. Refuses an empty
// or out-of-bounds rect, images over kMaxDecodePixels, and corrupt data.
// Never throws.
[[nodiscard]] JpegCropResult crop_jpeg(std::span<const std::byte> bytes,
                                       const PixelRect& rect) noexcept;

}  // namespace pptctl
