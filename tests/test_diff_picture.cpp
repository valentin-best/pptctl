// Tests for the picture-level diff (Phase 4). Owned by the picture diff issue; keep
// picture-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <pptctl/diff.hpp>

TEST_CASE("Diff/picture: scaffold compiles") {
    pptctl::SlideChanges c;
    CHECK(c.empty());
}
