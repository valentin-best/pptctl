// pptctl tests — doctest main.
//
// Each test_*.cpp file uses DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN is NOT set,
// because we want a single main() here. Test files just include doctest.h
// and write TEST_CASE blocks.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
