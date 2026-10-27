// optimize_crop.cpp — the "crop" pass: cropped-out image regions (#104).

#include "optimize_internal.hpp"

namespace pptctl::detail {

// No-op until #104 lands.
PassResult optimize_crop(const Package& /*pkg*/,
                         const PartGraph& /*graph*/,
                         const OptimizeOptions& /*opts*/) {
    return {};
}

}  // namespace pptctl::detail
