// Area-average downsampling — see resample.hpp.
//
// Separable: a horizontal pass into a float buffer (source rows x output
// columns), then a vertical pass into the output. Each output index along an
// axis covers the source interval [o * scale, (o + 1) * scale); its taps are
// the source indices that interval overlaps, weighted by the overlap length
// over the interval's length, so the weights sum to 1.

#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pptctl::detail {

namespace {

struct Taps {
    std::size_t first = 0;       // first source index
    std::vector<float> weights;  // one per source index from `first`
};

std::vector<Taps> taps_for(std::uint32_t in, std::uint32_t out) {
    std::vector<Taps> taps(out);
    const double scale = static_cast<double>(in) / static_cast<double>(out);
    for (std::uint32_t o = 0; o < out; ++o) {
        const double start = static_cast<double>(o) * scale;
        const double end = std::min(static_cast<double>(in), static_cast<double>(o + 1) * scale);
        const auto first = static_cast<std::size_t>(std::floor(start));
        const auto last =
            std::min(static_cast<std::size_t>(in), static_cast<std::size_t>(std::ceil(end)));
        Taps& t = taps[o];
        t.first = first;
        for (std::size_t i = first; i < last; ++i) {
            const double lo = std::max(start, static_cast<double>(i));
            const double hi = std::min(end, static_cast<double>(i + 1));
            t.weights.push_back(static_cast<float>(std::max(0.0, hi - lo) / scale));
        }
    }
    return taps;
}

std::uint8_t to_byte(float v) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

}  // namespace

Image downsample_area(const Image& src, std::uint32_t width, std::uint32_t height) {
    Image out = src;  // metadata
    out.width = std::clamp<std::uint32_t>(width, 1, src.width);
    out.height = std::clamp<std::uint32_t>(height, 1, src.height);
    const std::size_t ch = src.channels;
    const bool alpha = src.has_alpha();
    const std::size_t a = ch - 1;  // alpha channel index when present

    // Source as floats, colour premultiplied by alpha (0-1).
    const auto sample = [&](std::size_t pixel, std::size_t c) {
        const float v = src.pixels[pixel * ch + c];
        if (!alpha || c == a) {
            return v;
        }
        return v * static_cast<float>(src.pixels[pixel * ch + a]) / 255.0F;
    };

    const auto htaps = taps_for(src.width, out.width);
    std::vector<float> rows(std::size_t{src.height} * out.width * ch);
    for (std::size_t y = 0; y < src.height; ++y) {
        for (std::size_t x = 0; x < out.width; ++x) {
            const Taps& t = htaps[x];
            float* dst = &rows[(y * out.width + x) * ch];
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const std::size_t pixel = y * src.width + t.first + k;
                for (std::size_t c = 0; c < ch; ++c) {
                    dst[c] += t.weights[k] * sample(pixel, c);
                }
            }
        }
    }

    const auto vtaps = taps_for(src.height, out.height);
    out.pixels.assign(std::size_t{out.width} * out.height * ch, 0);
    std::vector<float> acc(ch);
    for (std::size_t y = 0; y < out.height; ++y) {
        const Taps& t = vtaps[y];
        for (std::size_t x = 0; x < out.width; ++x) {
            std::fill(acc.begin(), acc.end(), 0.0F);
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const float* row = &rows[((t.first + k) * out.width + x) * ch];
                for (std::size_t c = 0; c < ch; ++c) {
                    acc[c] += t.weights[k] * row[c];
                }
            }
            std::uint8_t* px = &out.pixels[(y * out.width + x) * ch];
            const float alpha_value = alpha ? acc[a] : 255.0F;
            for (std::size_t c = 0; c < ch; ++c) {
                if (!alpha || c == a) {
                    px[c] = to_byte(acc[c]);
                } else {
                    // Un-premultiply; a fully transparent pixel's colour is moot.
                    px[c] = alpha_value > 0.0F ? to_byte(acc[c] * 255.0F / alpha_value) : 0;
                }
            }
        }
    }
    return out;
}

}  // namespace pptctl::detail
