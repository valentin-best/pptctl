// optimize_images.cpp — the "images" pass: image recompression and downsampling (#103).

#include "optimize_internal.hpp"

namespace pptctl::detail {

// No-op until #103 lands.
PassResult optimize_images(const Package& /*pkg*/,
                           const PartGraph& /*graph*/,
                           const OptimizeOptions& /*opts*/) {
    return {};
}

}  // namespace pptctl::detail
