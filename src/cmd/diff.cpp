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

// One side of the diff: the open package, its slide part names (by 1-based
// index - 1), and every slide reduced to its summary. Summaries are built one
// slide in memory at a time; the package stays open so changed pairs can be
// re-loaded later.
struct Deck {
    Package pkg;
    std::vector<std::string> parts;
    std::vector<SlideSummary> summaries;

    bool open(const std::string& path) {
        if (!pkg.open(path)) {
            eprintln("error: cannot open '{}'", path);
            return false;
        }
        Presentation pres(pkg);
        if (!pres.load()) {
            eprintln("error: '{}' is not a valid .pptx file", path);
            return false;
        }
        parts = pres.slide_parts();
        summaries.reserve(parts.size());
        std::size_t n = 0;
        for (const auto& part : parts) {
            ++n;
            Slide slide;
            if (!slide.load(pkg, part)) {
                eprintln("warning: could not read {} in '{}'", part, path);
                continue;
            }
            slide.set_index(n);
            summaries.push_back(summarize(slide));
        }
        return true;
    }
};

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

std::optional<DiffReport> diff_report(const std::string& a,
                                      const std::string& b,
                                      const DiffOptions& opts) {
    Deck da;
    Deck db;
    if (!da.open(a) || !db.open(b)) {
        return std::nullopt;
    }
    DiffReport report = match_slides(da.summaries, db.summaries, opts);

    // Second pass: only the pairs that matched but differ are re-loaded and
    // compared shape by shape. Unchanged/added/removed slides are never
    // parsed twice.
    for (auto& s : report.slides) {
        if (s.status != SlideStatus::Changed) {
            continue;
        }
        Slide sa;
        Slide sb;
        if (!sa.load(da.pkg, da.parts[s.a_index - 1]) ||
            !sb.load(db.pkg, db.parts[s.b_index - 1])) {
            continue;
        }
        sa.set_index(s.a_index);
        sb.set_index(s.b_index);
        s.changes = compare_slides(sa, sb, opts);
    }
    return report;
}

int diff_to(const std::string& a, const std::string& b, std::ostream& os, const DiffOptions& opts) {
    const auto report = diff_report(a, b, opts);
    if (!report) {
        return 2;
    }
    print_report(*report, os);
    return report->identical() ? 0 : 1;
}

int diff(const std::string& a, const std::string& b, const DiffOptions& opts) {
    return diff_to(a, b, std::cout, opts);
}

}  // namespace pptctl::cmd
