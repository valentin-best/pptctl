// optimize_layouts.cpp — the "layouts" pass: unused slide layouts and masters (#100).

#include "optimize_internal.hpp"

namespace pptctl::detail {

// No-op until #100 lands.
PassResult optimize_layouts(const Package& /*pkg*/,
                            const PartGraph& /*graph*/,
                            const OptimizeOptions& /*opts*/) {
    return {};
}

}  // namespace pptctl::detail
