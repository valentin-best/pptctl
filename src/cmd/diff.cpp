// pptctl diff — semantic diff between two .pptx files.
//
// Exit codes follow `git diff`: 0 identical, 1 differences found, 2 error.

#include <format>
#include <iostream>
#include <optional>
#include <ostream>
#include <pptctl/diff.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <vector>

namespace pptctl::cmd {

namespace {

// Reduce every slide of a deck to its summary, one slide in memory at a time.
std::optional<std::vector<SlideSummary>> summarize_deck(const std::string& path) {
    Package pkg;
    if (!pkg.open(path)) {
        eprintln("error: cannot open '{}'", path);
        return std::nullopt;
    }
    Presentation pres(pkg);
    if (!pres.load()) {
        eprintln("error: '{}' is not a valid .pptx file", path);
        return std::nullopt;
    }
    std::vector<SlideSummary> out;
    out.reserve(pres.slide_parts().size());
    std::size_t n = 0;
    for (const auto& part : pres.slide_parts()) {
        ++n;
        Slide slide;
        if (!slide.load(pkg, part)) {
            eprintln("warning: could not read {} in '{}'", part, path);
            continue;
        }
        slide.set_index(n);
        out.push_back(summarize(slide));
    }
    return out;
}

// Minimal terminal rendering: one line per slide that is not simply unchanged.
// The full --format unified|json|html renderers land with #71.
void print_report(const DiffReport& report, std::ostream& os) {
    for (const auto& s : report.slides) {
        switch (s.status) {
            case SlideStatus::Added:
                os << std::format("slide {}: added\n", s.b_index);
                break;
            case SlideStatus::Removed:
                os << std::format("slide {}: removed\n", s.a_index);
                break;
            case SlideStatus::Changed:
            case SlideStatus::Unchanged: {
                if (s.status == SlideStatus::Unchanged && !s.moved) {
                    break;
                }
                std::string what = s.moved ? "moved" : "";
                if (s.status == SlideStatus::Changed) {
                    what += what.empty() ? "" : ", ";
                    what += std::format("changed (similarity {:.2f})", s.similarity);
                }
                os << std::format("slide {} -> {}: {}\n", s.a_index, s.b_index, what);
                break;
            }
        }
    }
}

}  // namespace

int diff_to(const std::string& a, const std::string& b, std::ostream& os, const DiffOptions& opts) {
    const auto sa = summarize_deck(a);
    if (!sa) {
        return 2;
    }
    const auto sb = summarize_deck(b);
    if (!sb) {
        return 2;
    }
    const DiffReport report = match_slides(*sa, *sb, opts);
    print_report(report, os);
    return report.identical() ? 0 : 1;
}

int diff(const std::string& a, const std::string& b, const DiffOptions& opts) {
    return diff_to(a, b, std::cout, opts);
}

}  // namespace pptctl::cmd
