// Tests for the shape-level diff (Phase 4). Owned by the shape diff issue; keep
// shape-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <pptctl/diff.hpp>

TEST_CASE("Diff/shape: scaffold compiles") {
    pptctl::SlideChanges c;
    CHECK(c.empty());
}
