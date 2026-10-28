// pptctl optimize — reduce a .pptx's size.
//
// Exit codes: 0 success (including a dry run), 1 error (bad arguments,
// unreadable input, failed write).
//
// Two renderers over one OptimizeReport: a per-pass summary (the default) and
// JSON (--json).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <pptctl/optimize.hpp>
#include <pptctl/print.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pptctl::cmd {

namespace {

namespace fs = std::filesystem;

std::string bytes(std::uint64_t n) {
    return human_bytes(static_cast<std::size_t>(n));
}

// Distinct details shown per kind before the rest is summarized as "+N more".
constexpr std::size_t kMaxDetails = 4;

// Values in first-seen order with their counts.
using Tally = std::vector<std::pair<std::string, int>>;

void count_in(Tally& tally, const std::string& value) {
    auto it = tally.begin();
    while (it != tally.end() && it->first != value) {
        ++it;
    }
    if (it == tally.end()) {
        tally.emplace_back(value, 1);
    } else {
        ++it->second;
    }
}

// " (layout x11, master, theme)": a kind's distinct details, or "" if none.
std::string details_of(const Tally& details) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& [detail, n] : details) {
        if (detail.empty()) {
            continue;
        }
        if (shown == kMaxDetails) {
            out += std::format(", +{} more", details.size() - shown);
            break;
        }
        out += (shown++ == 0 ? "" : ", ") + detail + (n > 1 ? std::format(" x{}", n) : "");
    }
    return out.empty() ? out : " (" + out + ")";
}

// "2 removed (Lato, Lato Bold), 1 recompressed, 37.2 MiB -> 8.6 MiB": action
// counts by kind in first-seen order, each with its details, then the pass's
// total before/after.
std::string pass_summary(const PassReport& pass) {
    if (pass.actions.empty()) {
        return "nothing to do";
    }
    Tally kinds;
    std::vector<Tally> details;  // parallel to kinds
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    for (const auto& a : pass.actions) {
        count_in(kinds, a.kind);
        details.resize(kinds.size());
        const auto k = static_cast<std::size_t>(
            std::find_if(
                kinds.begin(), kinds.end(), [&](const auto& e) { return e.first == a.kind; }) -
            kinds.begin());
        count_in(details[k], a.detail);
        before += a.bytes_before;
        after += a.bytes_after;
    }
    std::string out;
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        out += std::format("{} {}{}, ", kinds[k].second, kinds[k].first, details_of(details[k]));
    }
    return out + std::format("{} -> {}", bytes(before), bytes(after));
}

std::string change_percent(std::uint64_t before, std::uint64_t after) {
    if (before == after || before == 0) {
        return "no change";
    }
    const double delta = static_cast<double>(after) - static_cast<double>(before);
    const auto pct = std::lround(100.0 * delta / static_cast<double>(before));
    if (pct == 0) {
        return delta < 0 ? "-<1%" : "+<1%";
    }
    return std::format("{:+}%", pct);
}

std::string target_of(const std::string& in, const OptimizeOutputOptions& out) {
    if (out.dry_run) {
        return {};
    }
    return out.in_place ? in : out.output;
}

void render_human(const OptimizeReport& report,
                  const std::string& in,
                  const OptimizeOutputOptions& out,
                  std::ostream& os) {
    if (out.dry_run) {
        println_to(os, "{} (dry run, nothing written)", in);
    } else if (out.in_place) {
        println_to(os, "{} (in place)", in);
    } else {
        println_to(os, "{} -> {}", in, out.output);
    }
    for (const auto& pass : report.passes) {
        println_to(os, "  {}: {}", pass.id, pass_summary(pass));
    }
    println_to(os,
               "total: {} -> {} ({})",
               bytes(report.input_bytes),
               bytes(report.output_bytes),
               change_percent(report.input_bytes, report.output_bytes));
}

void render_json(const OptimizeReport& report,
                 const std::string& in,
                 const OptimizeOutputOptions& out,
                 std::ostream& os) {
    nlohmann::json j;
    j["schema_version"] = 1;
    j["input"] = in;
    const auto target = target_of(in, out);
    j["output"] = target.empty() ? nlohmann::json(nullptr) : nlohmann::json(target);
    j["dry_run"] = out.dry_run;
    j["input_bytes"] = report.input_bytes;
    j["output_bytes"] = report.output_bytes;
    auto passes = nlohmann::json::array();
    for (const auto& pass : report.passes) {
        auto actions = nlohmann::json::array();
        for (const auto& a : pass.actions) {
            actions.push_back({{"part", a.part},
                               {"kind", a.kind},
                               {"detail", a.detail},
                               {"bytes_before", a.bytes_before},
                               {"bytes_after", a.bytes_after}});
        }
        passes.push_back({{"id", pass.id}, {"actions", std::move(actions)}});
    }
    j["passes"] = std::move(passes);
    os << j.dump(2) << '\n';
}

// True if `out` names the same existing file as `in`.
bool same_file(const std::string& in, const std::string& out) {
    std::error_code ec;
    return fs::equivalent(in, out, ec);
}

}  // namespace

// Render a computed report. Exposed so tests can drive the renderer with
// hand-built reports.
void render_optimize(const OptimizeReport& report,
                     const std::string& in,
                     const OptimizeOutputOptions& out,
                     std::ostream& os) {
    if (out.json) {
        render_json(report, in, out, os);
    } else {
        render_human(report, in, out, os);
    }
}

int optimize_to(const std::string& in,
                const OptimizeOptions& opts,
                const OptimizeOutputOptions& out,
                std::ostream& os) {
    if (out.in_place && !out.output.empty()) {
        eprintln("error: -o and --in-place are mutually exclusive");
        return 1;
    }
    if (!out.in_place && !out.dry_run && out.output.empty()) {
        eprintln("error: an output is required: -o <file>, --in-place or --dry-run");
        return 1;
    }
    if (!out.in_place && !out.output.empty() && same_file(in, out.output)) {
        eprintln("error: '{}' is the input; use --in-place to overwrite it", out.output);
        return 1;
    }
    std::string error;
    const auto report = optimize(in, target_of(in, out), opts, error);
    if (!report) {
        eprintln("error: {}", error);
        return 1;
    }
    render_optimize(*report, in, out, os);
    return 0;
}

int optimize(const std::string& in, const OptimizeOptions& opts, const OptimizeOutputOptions& out) {
    return optimize_to(in, opts, out, std::cout);
}

}  // namespace pptctl::cmd
