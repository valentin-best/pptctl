// Tests for the text-level diff (Phase 4). Owned by the text diff issue; keep
// text-specific cases here rather than in test_diff.cpp.

#include <doctest/doctest.h>

#include <pptctl/diff.hpp>

TEST_CASE("Diff/text: scaffold compiles") {
    pptctl::SlideChanges c;
    CHECK(c.empty());
}
