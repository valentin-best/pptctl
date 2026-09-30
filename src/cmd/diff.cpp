// pptctl diff — semantic diff between two .pptx files.
//
// Exit codes follow `git diff`: 0 identical, 1 differences found, 2 error.
//
// Three renderers over one DiffReport (#71): unified (terminal, the default),
// json (docs/diff-schema.md) and html (self-contained page). The nested op
// families (text/shape/picture) reach each renderer through exactly one
// function — render_changes_unified / changes_to_json / render_changes_html —
// so wiring a new op family is a few lines per format.

#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <pptctl/diff.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <pptctl/print.hpp>
#include <pptctl/slide.hpp>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

// ── shared ────────────────────────────────────────────────────────────────────

enum class Format : std::uint8_t { Unified, Json, Html };

std::optional<Format> parse_format(std::string_view s) {
    if (s == "unified") {
        return Format::Unified;
    }
    if (s == "json") {
        return Format::Json;
    }
    if (s == "html") {
        return Format::Html;
    }
    return std::nullopt;
}

std::string_view status_label(SlideStatus s) {
    switch (s) {
        case SlideStatus::Unchanged:
            return "unchanged";
        case SlideStatus::Changed:
            return "changed";
        case SlideStatus::Added:
            return "added";
        case SlideStatus::Removed:
            return "removed";
    }
    return "unchanged";
}

struct Counts {
    std::size_t unchanged = 0;
    std::size_t changed = 0;
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t moved = 0;
};

Counts count(const DiffReport& report) {
    Counts c;
    for (const auto& s : report.slides) {
        switch (s.status) {
            case SlideStatus::Unchanged:
                ++c.unchanged;
                break;
            case SlideStatus::Changed:
                ++c.changed;
                break;
            case SlideStatus::Added:
                ++c.added;
                break;
            case SlideStatus::Removed:
                ++c.removed;
                break;
        }
        if (s.moved) {
            ++c.moved;
        }
    }
    return c;
}

// "N slides: A added, R removed, M moved, C changed" — N counts every entry
// of the report (the union of both decks), like the rows of the JSON/HTML.
std::string summary_line(const DiffReport& report) {
    const Counts c = count(report);
    return std::format("{} slides: {} added, {} removed, {} moved, {} changed",
                       report.slides.size(),
                       c.added,
                       c.removed,
                       c.moved,
                       c.changed);
}

// The parenthesised tag of a hunk header: "moved", "changed 0.83", or both.
std::string slide_tag(const SlideDiff& s) {
    switch (s.status) {
        case SlideStatus::Added:
            return "added";
        case SlideStatus::Removed:
            return "removed";
        case SlideStatus::Unchanged:
        case SlideStatus::Changed:
            break;
    }
    std::string tag = s.moved ? "moved" : "";
    if (s.status == SlideStatus::Changed) {
        tag += tag.empty() ? "" : ", ";
        tag += std::format("changed {:.2f}", s.similarity);
    }
    return tag;
}

// A slide worth a hunk in the terminal view: anything but an in-place exact match.
bool noteworthy(const SlideDiff& s) noexcept {
    return s.status != SlideStatus::Unchanged || s.moved;
}

// ── unified ───────────────────────────────────────────────────────────────────

struct Palette {
    std::string_view bold;
    std::string_view cyan;
    std::string_view green;
    std::string_view red;
    std::string_view reset;
};

constexpr Palette kAnsi{"\x1b[1m", "\x1b[36m", "\x1b[32m", "\x1b[31m", "\x1b[0m"};
constexpr Palette kPlain{"", "", "", "", ""};

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

// --color / --no-color win; otherwise colour only a real terminal that has not
// opted out via NO_COLOR (https://no-color.org).
bool want_color(const DiffOutputOptions& out, const std::ostream& os) {
    if (out.color) {
        return *out.color;
    }
    if (&os != &std::cout || !stdout_is_tty()) {
        return false;
    }
    const char* no_color = std::getenv("NO_COLOR");
    return no_color == nullptr || *no_color == '\0';
}

// Nested ops of one changed slide, as hunk body lines. Text ops are the
// `+`/`-` lines; shape and picture ops are one-line summaries.
void render_changes_unified([[maybe_unused]] const SlideChanges& changes,
                            [[maybe_unused]] std::ostream& os,
                            [[maybe_unused]] const Palette& p) {
    // TODO(#70/#72/#73): once each family declares describe(), emit one line
    // per op here, e.g.
    //   for (const auto& t : changes.text)     { os << p.green << '+' ... }
    //   for (const auto& s : changes.shapes)   { os << "  " << describe(s) << '\n'; }
    //   for (const auto& c : changes.pictures) { os << "  " << describe(c) << '\n'; }
}

void render_unified(const DiffReport& report,
                    const std::string& a,
                    const std::string& b,
                    std::ostream& os,
                    const Palette& p) {
    if (report.identical()) {
        return;
    }
    os << std::format("{}--- {}{}\n", p.bold, a, p.reset);
    os << std::format("{}+++ {}{}\n", p.bold, b, p.reset);
    for (const auto& s : report.slides) {
        if (!noteworthy(s)) {
            continue;
        }
        std::string where;
        switch (s.status) {
            case SlideStatus::Added:
                where = std::format("slide {}", s.b_index);
                break;
            case SlideStatus::Removed:
                where = std::format("slide {}", s.a_index);
                break;
            case SlideStatus::Unchanged:
            case SlideStatus::Changed:
                where = std::format("slide {} -> {}", s.a_index, s.b_index);
                break;
        }
        os << std::format("{}@@ {} ({}) @@{}\n", p.cyan, where, slide_tag(s), p.reset);
        render_changes_unified(s.changes, os, p);
    }
    os << summary_line(report) << '\n';
}

// ── json ──────────────────────────────────────────────────────────────────────

nlohmann::json index_or_null(std::size_t i) {
    return i == 0 ? nlohmann::json(nullptr) : nlohmann::json(i);
}

// Nested ops of one slide as the `changes` object of docs/diff-schema.md.
nlohmann::json changes_to_json([[maybe_unused]] const SlideChanges& changes) {
    nlohmann::json j;
    j["text"] = nlohmann::json::array();
    j["shapes"] = nlohmann::json::array();
    j["pictures"] = nlohmann::json::array();
    // TODO(#70/#72/#73): once each family declares to_json(), fill the arrays:
    //   for (const auto& t : changes.text)     { j["text"].push_back(to_json(t)); }
    //   for (const auto& s : changes.shapes)   { j["shapes"].push_back(to_json(s)); }
    //   for (const auto& c : changes.pictures) { j["pictures"].push_back(to_json(c)); }
    return j;
}

void render_json(const DiffReport& report,
                 const std::string& a,
                 const std::string& b,
                 std::ostream& os) {
    nlohmann::json out;
    out["schema_version"] = 1;
    out["a"] = a;
    out["b"] = b;
    auto slides = nlohmann::json::array();
    for (const auto& s : report.slides) {
        nlohmann::json j;
        j["status"] = status_label(s.status);
        j["a_index"] = index_or_null(s.a_index);
        j["b_index"] = index_or_null(s.b_index);
        j["moved"] = s.moved;
        j["similarity"] = s.similarity;
        j["changes"] = changes_to_json(s.changes);
        slides.push_back(std::move(j));
    }
    out["slides"] = std::move(slides);
    const Counts c = count(report);
    out["summary"] = {{"unchanged", c.unchanged},
                      {"changed", c.changed},
                      {"added", c.added},
                      {"removed", c.removed},
                      {"moved", c.moved}};
    os << out.dump(2) << '\n';
}

// ── html ──────────────────────────────────────────────────────────────────────

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += ch;
        }
    }
    return out;
}

// Inline stylesheet: no external assets, renders in any browser. Kept plain
// on purpose; the page is a review hand-off, not a product.
constexpr std::string_view kHtmlStyle = R"(
:root { color-scheme: light dark; }
body { font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; margin: 2rem auto;
       max-width: 72rem; padding: 0 1rem; color: #1f2328; background: #fff; }
h1 { font-size: 1.25rem; margin: 0 0 .25rem; }
code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: .9em; }
.files { color: #57606a; margin: 0 0 .5rem; }
.summary { margin: 0 0 1.5rem; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; vertical-align: top; padding: .4rem .6rem; border-top: 1px solid #d0d7de; }
th { font-weight: 600; color: #57606a; border-top: 0; }
td.idx { width: 4rem; text-align: right; font-variant-numeric: tabular-nums; }
.badge { display: inline-block; padding: .05rem .5rem; border-radius: 1rem; font-size: .8em;
         font-weight: 600; }
.badge.unchanged { background: #eaeef2; color: #57606a; }
.badge.changed { background: #fff8c5; color: #7d4e00; }
.badge.added { background: #dafbe1; color: #1a7f37; }
.badge.removed { background: #ffebe9; color: #cf222e; }
.moved { color: #8250df; font-size: .85em; margin-left: .4rem; }
.similarity { color: #57606a; font-size: .85em; margin-left: .4rem; }
ul.ops { margin: 0; padding-left: 1.2rem; }
ul.ops li.add { color: #1a7f37; }
ul.ops li.del { color: #cf222e; }
tr.added td { background: #f0fff4; }
tr.removed td { background: #fff5f5; }
tr.changed td { background: #fffdf0; }
@media (prefers-color-scheme: dark) {
  body { color: #e6edf3; background: #0d1117; }
  th, td { border-color: #30363d; }
  th, .files, .similarity { color: #8b949e; }
  .badge.unchanged { background: #21262d; color: #8b949e; }
  .badge.changed { background: #3b2e00; color: #e3b341; }
  .badge.added { background: #0f2e18; color: #3fb950; }
  .badge.removed { background: #3d1418; color: #ff7b72; }
  tr.added td { background: #0f1f14; }
  tr.removed td { background: #23151a; }
  tr.changed td { background: #1f1c0c; }
}
)";

// Nested ops of one changed slide as <li> items of its row's op list.
void render_changes_html([[maybe_unused]] const SlideChanges& changes,
                         [[maybe_unused]] std::ostream& os) {
    // TODO(#70/#72/#73): once each family declares describe(), emit one <li>
    // per op here (html_escape every string), e.g.
    //   for (const auto& s : changes.shapes) {
    //       os << "<li>" << html_escape(describe(s)) << "</li>\n";
    //   }
}

void render_html(const DiffReport& report,
                 const std::string& a,
                 const std::string& b,
                 std::ostream& os) {
    os << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
          "<title>pptctl diff</title>\n<style>"
       << kHtmlStyle << "</style>\n</head>\n<body>\n<h1>pptctl diff</h1>\n";
    os << std::format(
        "<p class=\"files\"><code class=\"path\">{}</code> &rarr; "
        "<code class=\"path\">{}</code></p>\n",
        html_escape(a),
        html_escape(b));
    os << std::format("<p class=\"summary\">{}</p>\n", html_escape(summary_line(report)));
    os << "<table>\n<thead><tr><th>A</th><th>B</th><th>Status</th><th>Changes</th></tr></thead>\n"
          "<tbody>\n";
    for (const auto& s : report.slides) {
        const std::string_view label = status_label(s.status);
        os << std::format("<tr class=\"{}\">", label);
        os << std::format("<td class=\"idx\">{}</td>",
                          s.a_index == 0 ? "" : std::to_string(s.a_index));
        os << std::format("<td class=\"idx\">{}</td>",
                          s.b_index == 0 ? "" : std::to_string(s.b_index));
        os << std::format("<td><span class=\"badge {}\">{}</span>", label, label);
        if (s.moved) {
            os << "<span class=\"moved\">moved</span>";
        }
        if (s.status == SlideStatus::Changed) {
            os << std::format("<span class=\"similarity\">{:.2f}</span>", s.similarity);
        }
        os << "</td><td>";
        if (!s.changes.empty()) {
            os << "<ul class=\"ops\">\n";
            render_changes_html(s.changes, os);
            os << "</ul>";
        }
        os << "</td></tr>\n";
    }
    os << "</tbody>\n</table>\n</body>\n</html>\n";
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

int diff_to(const std::string& a,
            const std::string& b,
            std::ostream& os,
            const DiffOptions& opts,
            const DiffOutputOptions& out) {
    const auto format = parse_format(out.format);
    if (!format) {
        eprintln("error: unknown format '{}' (expected unified, json or html)", out.format);
        return 2;
    }
    const auto report = diff_report(a, b, opts);
    if (!report) {
        return 2;
    }
    if (!out.quiet) {
        switch (*format) {
            case Format::Unified:
                render_unified(*report, a, b, os, want_color(out, os) ? kAnsi : kPlain);
                break;
            case Format::Json:
                render_json(*report, a, b, os);
                break;
            case Format::Html:
                render_html(*report, a, b, os);
                break;
        }
    }
    return report->identical() ? 0 : 1;
}

int diff(const std::string& a,
         const std::string& b,
         const DiffOptions& opts,
         const DiffOutputOptions& out) {
    return diff_to(a, b, std::cout, opts, out);
}

}  // namespace pptctl::cmd
