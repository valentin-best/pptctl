// optimize_fonts.cpp — the "fonts" pass: embedded fonts (#101).

#include "optimize_internal.hpp"

namespace pptctl::detail {

// No-op until #101 lands.
PassResult optimize_fonts(const Package& /*pkg*/,
                          const PartGraph& /*graph*/,
                          const OptimizeOptions& /*opts*/) {
    return {};
}

}  // namespace pptctl::detail
