// pptctl/merge.hpp
//
// Merge engine: combine two or more decks into one. The output is written
// through Package::write on a base package, so the base deck's parts are
// copied raw and everything taken from the other decks arrives as added parts
// (renamed where a name is taken, with relationships and content types
// rebuilt). The base is the --theme deck, or deck 1 when each deck keeps its
// own formatting. Slides appear in input order whichever deck is the base.
// The human summary and JSON output of `pptctl merge` are renderers of
// MergeReport.
//
// Pipeline stages and the issues that fill them in:
//   import       — copy each slide's part closure into the base (#135)
//   concatenate  — presentation.xml slide list, masters, notes master (#136)
//   media dedupe — one copy of each identical media part (#137)
//   theme        — --theme=N restyles every slide onto deck N's layouts (#138)
//   masters      — collapse identical slide masters (#139)
// Until import lands, the output is the base deck unchanged.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pptctl {

struct MergeOptions {
    // 1-based input whose slide masters and layouts every slide is restyled
    // onto (--theme=N); 0 keeps each deck's own formatting.
    std::size_t theme = 0;
};

// What merging one input contributed to the output.
struct MergeInputReport {
    std::string path;
    bool base = false;                    // the package the output is written on
    std::uint64_t bytes = 0;              // input file size
    std::size_t slides = 0;               // slides taken into the output
    std::size_t parts_added = 0;          // parts added to the base package
    std::size_t media_deduplicated = 0;   // media parts replaced by an identical earlier one
    std::uint64_t media_bytes_saved = 0;  // their total uncompressed size
    std::size_t masters_reused = 0;       // masters collapsed onto an identical earlier one
    std::size_t masters_imported = 0;     // masters added to the base package
    std::vector<std::string> warnings;    // what didn't carry over cleanly
};

struct MergeReport {
    std::vector<MergeInputReport> inputs;  // in input order
    std::size_t slides = 0;                // slides in the output
    std::uint64_t output_bytes = 0;        // output file size (also computed on a dry run)
};

// Merge the decks at `in_paths` (two or more) into `out_path`; an empty
// `out_path` is a dry run that measures the result without writing it there.
// Returns nullopt and sets `error` when an input can't be opened as a
// presentation, `opts.theme` is out of range, or a write fails.
[[nodiscard]] std::optional<MergeReport> merge(std::span<const std::string> in_paths,
                                               const std::string& out_path,
                                               const MergeOptions& opts,
                                               std::string& error);

// Output settings of the merge subcommand.
struct MergeOutputOptions {
    std::string output;    // -o; required unless dry_run
    bool dry_run = false;  // compute and print the report, write nothing
    bool json = false;     // machine-readable JSON instead of the summary
};

}  // namespace pptctl
