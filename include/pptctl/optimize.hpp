// pptctl/optimize.hpp
//
// Optimize engine: a fixed pipeline of size-reduction passes over a package,
// and the report they fill in. Each pass reads a package plus its relationship
// graph and returns the parts it wants changed; the pipeline completes the
// bookkeeping every removal needs (rels, content types), applies the changes,
// and hands the result to the next pass. The human summary and JSON output of
// `pptctl optimize` are renderers of OptimizeReport.
//
// Passes and their ids, in run order (each in src/lib/optimize_<id>.cpp):
//   layouts  — unused slide layouts and masters (#100)
//   fonts    — embedded fonts (#101)
//   crop     — cropped-out image regions (#104)
//   images   — image recompression and downsampling (#103)
//   orphans  — media nothing references (#99)
// Layouts run first so the media only they used is orphaned by the time the
// orphans pass runs; crop runs before images so images downsample to the
// visible region.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <pptctl/package.hpp>
#include <pptctl/part_graph.hpp>
#include <span>
#include <string>
#include <vector>

namespace pptctl {

// Which passes run. All are on by default; the CLI exposes one opt-out each
// (--no-layouts, --keep-fonts, --no-crop, --no-images, --no-orphans).
struct OptimizeOptions {
    bool layouts = true;
    bool fonts = true;
    bool crop = true;
    bool images = true;
    bool orphans = true;
};

// One thing a pass did to one part. Sizes are uncompressed part bytes.
struct OptimizeAction {
    std::string part;                // part name, e.g. "ppt/media/image3.jpeg"
    std::string kind;                // past-tense verb the summary groups by: "removed", ...
    std::string detail;              // optional, e.g. "4000x3000 -> 1920x1440" or a font name
    std::uint64_t bytes_before = 0;  // part size before the pass
    std::uint64_t bytes_after = 0;   // part size after; 0 when removed
};

struct PassReport {
    std::string id;  // pass id, e.g. "images"
    std::vector<OptimizeAction> actions;
};

struct OptimizeReport {
    std::vector<PassReport> passes;  // enabled passes only, in run order
    std::uint64_t input_bytes = 0;   // input file size
    std::uint64_t output_bytes = 0;  // resulting file size (also computed on a dry run)
};

// What a pass hands back: the parts to change and what it did to them. A pass
// lists the parts it removes; the pipeline completes those removals (see
// complete_removals), so a pass only edits the parts whose content it changes,
// including any r:id references to removed parts inside them.
struct PassResult {
    PartChanges changes;
    std::vector<OptimizeAction> actions;
};

using OptimizePass =
    std::function<PassResult(const Package&, const PartGraph&, const OptimizeOptions&)>;

struct RegisteredPass {
    std::string id;
    bool OptimizeOptions::*enabled;  // the option that turns this pass on
    OptimizePass run;
};

// The built-in passes, in run order.
[[nodiscard]] const std::vector<RegisteredPass>& optimize_passes();

// Complete the removals in `changes` against `package`: also remove each
// removed part's own .rels, drop the relationships that target a removed part
// from every reachable source's .rels (rewriting the pass's replacement when it
// supplied one), and drop removed parts' content-type Overrides. A .rels or
// `[Content_Types].xml` is only rewritten when something in it actually goes.
[[nodiscard]] PartChanges complete_removals(const Package& package,
                                            const PartGraph& graph,
                                            PartChanges changes);

// Run the enabled passes in order over the package at `in_path`. Each pass
// sees the package as the previous passes left it: after a pass that changed
// anything, the result is written to a temporary file and reopened. The final
// package is written to `out_path`, which may be `in_path`; an empty
// `out_path` is a dry run that writes nothing there. Returns nullopt and sets
// `error` when the input can't be opened or a write fails.
[[nodiscard]] std::optional<OptimizeReport> optimize(const std::string& in_path,
                                                     const std::string& out_path,
                                                     const OptimizeOptions& opts,
                                                     std::string& error,
                                                     std::span<const RegisteredPass> passes);

[[nodiscard]] std::optional<OptimizeReport> optimize(const std::string& in_path,
                                                     const std::string& out_path,
                                                     const OptimizeOptions& opts,
                                                     std::string& error);

// Output settings of the optimize subcommand.
struct OptimizeOutputOptions {
    std::string output;     // -o; required unless in_place or dry_run
    bool in_place = false;  // overwrite the input (temp file + rename)
    bool dry_run = false;   // compute and print the report, write nothing
    bool json = false;      // machine-readable JSON instead of the summary
};

}  // namespace pptctl
