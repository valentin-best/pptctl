// optimize_orphans.cpp — the "orphans" pass: orphan media (#99).

#include "optimize_internal.hpp"

namespace pptctl::detail {

// No-op until #99 lands.
PassResult optimize_orphans(const Package& /*pkg*/,
                            const PartGraph& /*graph*/,
                            const OptimizeOptions& /*opts*/) {
    return {};
}

}  // namespace pptctl::detail
