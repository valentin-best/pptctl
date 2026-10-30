// pptctl merge — combine decks into one.
//
// Exit codes: 0 success (including a dry run), 1 error (bad arguments,
// unreadable input, failed write).
//
// Two renderers over one MergeReport: a per-input summary (the default) and
// JSON (--json).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <pptctl/merge.hpp>
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

std::string plural(std::size_t n, const char* noun) {
    return std::format("{} {}{}", n, noun, n == 1 ? "" : "s");
}

// "4 slides, 12 parts added, 2 media deduplicated (340.0 KiB), 1 master
// imported, 1 master reused": the slide count, then only the non-zero rest.
std::string input_summary(const MergeInputReport& input) {
    auto out = plural(input.slides, "slide");
    if (input.base) {
        return out + " (base)";
    }
    if (input.parts_added > 0) {
        out += ", " + plural(input.parts_added, "part") + " added";
    }
    if (input.media_deduplicated > 0) {
        out += std::format(", {} media deduplicated ({})",
                           input.media_deduplicated,
                           bytes(input.media_bytes_saved));
    }
    if (input.masters_imported > 0) {
        out += ", " + plural(input.masters_imported, "master") + " imported";
    }
    if (input.masters_reused > 0) {
        out += ", " + plural(input.masters_reused, "master") + " reused";
    }
    return out;
}

void render_human(const MergeReport& report, const MergeOutputOptions& out, std::ostream& os) {
    if (out.dry_run) {
        println_to(os, "merge (dry run, nothing written)");
    } else {
        println_to(os, "merge -> {}", out.output);
    }
    for (std::size_t i = 0; i < report.inputs.size(); ++i) {
        const auto& input = report.inputs[i];
        println_to(os, "  {}. {}: {}", i + 1, input.path, input_summary(input));
        for (const auto& w : input.warnings) {
            println_to(os, "     warning: {}", w);
        }
    }
    println_to(os, "total: {}, {}", plural(report.slides, "slide"), bytes(report.output_bytes));
}

void render_json(const MergeReport& report,
                 const MergeOptions& opts,
                 const MergeOutputOptions& out,
                 std::ostream& os) {
    nlohmann::json j;
    j["schema_version"] = 1;
    j["output"] =
        out.dry_run || out.output.empty() ? nlohmann::json(nullptr) : nlohmann::json(out.output);
    j["dry_run"] = out.dry_run;
    j["theme"] = opts.theme == 0 ? nlohmann::json(nullptr) : nlohmann::json(opts.theme);
    j["slides"] = report.slides;
    j["output_bytes"] = report.output_bytes;
    auto inputs = nlohmann::json::array();
    for (const auto& input : report.inputs) {
        inputs.push_back(
            {{"path", input.path},
             {"base", input.base},
             {"bytes", input.bytes},
             {"slides", input.slides},
             {"parts_added", input.parts_added},
             {"media_deduplicated",
              {{"count", input.media_deduplicated}, {"bytes", input.media_bytes_saved}}},
             {"masters", {{"imported", input.masters_imported}, {"reused", input.masters_reused}}},
             {"warnings", input.warnings}});
    }
    j["inputs"] = std::move(inputs);
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
void render_merge(const MergeReport& report,
                  const MergeOptions& opts,
                  const MergeOutputOptions& out,
                  std::ostream& os) {
    if (out.json) {
        render_json(report, opts, out, os);
    } else {
        render_human(report, out, os);
    }
}

int merge_to(const std::vector<std::string>& inputs,
             const MergeOptions& opts,
             const MergeOutputOptions& out,
             std::ostream& os) {
    if (inputs.size() < 2) {
        eprintln("error: merge needs at least two input decks");
        return 1;
    }
    if (!out.dry_run && out.output.empty()) {
        eprintln("error: an output is required: -o <file> or --dry-run");
        return 1;
    }
    if (opts.theme > inputs.size()) {
        eprintln("error: --theme={} is out of range: there are {} input decks",
                 opts.theme,
                 inputs.size());
        return 1;
    }
    if (!out.output.empty()) {
        for (const auto& in : inputs) {
            if (same_file(in, out.output)) {
                eprintln("error: '{}' is one of the inputs", out.output);
                return 1;
            }
        }
    }
    std::string error;
    const auto report = merge(inputs, out.dry_run ? std::string{} : out.output, opts, error);
    if (!report) {
        eprintln("error: {}", error);
        return 1;
    }
    render_merge(*report, opts, out, os);
    return 0;
}

int merge(const std::vector<std::string>& inputs,
          const MergeOptions& opts,
          const MergeOutputOptions& out) {
    return merge_to(inputs, opts, out, std::cout);
}

}  // namespace pptctl::cmd
