// Tests for the "crop" optimize pass (#104).

#include <doctest/doctest.h>

#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <string>

#include "../src/lib/optimize_internal.hpp"

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

TEST_CASE("optimize crop: runs on a minimal deck") {
    pptctl::Package pkg;
    REQUIRE(pkg.open(std::string(PPTCTL_FIXTURES_DIR) + "/minimal.pptx"));
    const auto graph = pptctl::PartGraph::build(pkg);
    CHECK_NOTHROW((void)pptctl::detail::optimize_crop(pkg, graph, {}));
}
