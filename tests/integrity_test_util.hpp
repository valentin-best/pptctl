// Shared helpers for the package-integrity rule tests
// (tests/test_integrity_<rule>.cpp): build a broken package from a clean
// fixture, then run one rule over it.

#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <pptctl/integrity.hpp>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifndef PPTCTL_FIXTURES_DIR
#define PPTCTL_FIXTURES_DIR "tests/fixtures"
#endif

namespace integrity_test {

namespace fs = std::filesystem;

inline std::string fixture(std::string_view name) {
    return std::string(PPTCTL_FIXTURES_DIR) + "/" + std::string(name);
}

// Every fixture deck that is meant to be well-formed: the integrity rules must
// stay silent on all of them.
inline std::vector<std::string> clean_fixtures() {
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(PPTCTL_FIXTURES_DIR)) {
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".pptx" && name != "integrity_problems.pptx") {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::transform(
        s.begin(), s.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

inline std::string to_string(const std::vector<std::byte>& b) {
    std::string out(b.size(), '\0');
    std::transform(
        b.begin(), b.end(), out.begin(), [](std::byte x) { return static_cast<char>(x); });
    return out;
}

constexpr std::string_view kRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

// A `.rels` part holding the given <Relationship> elements.
inline std::vector<std::byte> rels(const std::vector<std::string>& entries) {
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
    for (const auto& e : entries) {
        xml += e;
    }
    xml += "</Relationships>";
    return to_bytes(xml);
}

// One <Relationship>; `type` is the last segment of an officeDocument
// relationship type ("image", "slideLayout", ...).
inline std::string rel(std::string_view id,
                       std::string_view type,
                       std::string_view target,
                       bool external = false) {
    return std::string(R"(<Relationship Id=")") + std::string(id) + R"(" Type=")" +
           std::string(kRelNs) + "/" + std::string(type) + R"(" Target=")" + std::string(target) +
           (external ? R"(" TargetMode="External"/>)" : R"("/>)");
}

// A fixture with `changes` applied, written to a scratch file that lives as
// long as this object, and opened.
struct EditedDeck {
    fs::path path;
    pptctl::Package pkg;

    EditedDeck(std::string_view fixture_name, const pptctl::PartChanges& changes) {
        static const std::string run = std::to_string(std::random_device{}());
        path = fs::temp_directory_path() /
               fs::path("pptctl_integrity_" + run + "_" + std::to_string(counter++) + ".pptx");
        pptctl::Package src;
        REQUIRE(src.open(fixture(fixture_name)));
        REQUIRE(src.write(path.string(), changes));
        REQUIRE(pkg.open(path.string()));
    }
    ~EditedDeck() {
        pkg = pptctl::Package{};
        std::error_code ec;
        fs::remove(path, ec);
    }
    EditedDeck(const EditedDeck&) = delete;
    EditedDeck& operator=(const EditedDeck&) = delete;
    EditedDeck(EditedDeck&&) = delete;
    EditedDeck& operator=(EditedDeck&&) = delete;

    static inline int counter = 0;
};

// The findings of the registered rule `id` alone over `pkg`.
inline std::vector<pptctl::Diagnostic> run_rule(std::string_view id, const pptctl::Package& pkg) {
    const auto graph = pptctl::PartGraph::build(pkg);
    std::vector<pptctl::Diagnostic> out;
    for (const auto& rule : pptctl::integrity_rules()) {
        if (rule.id == id) {
            rule.fn(pkg, graph, {}, out);
        }
    }
    return out;
}

inline std::vector<pptctl::Diagnostic> run_rule(std::string_view id, const std::string& path) {
    pptctl::Package pkg;
    REQUIRE(pkg.open(path));
    return run_rule(id, pkg);
}

}  // namespace integrity_test
