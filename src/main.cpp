// pptctl — a CLI for .pptx files.
//
// This file is just the dispatch layer. All actual logic lives in
// src/cmd/*.cpp and the internal library in src/lib/.

#include <CLI/CLI.hpp>
#include <ostream>
#include <pptctl/diff.hpp>
#include <pptctl/lint.hpp>
#include <pptctl/merge.hpp>
#include <pptctl/optimize.hpp>
#include <pptctl/print.hpp>
#include <string>
#include <vector>

namespace pptctl::cmd {

// Subcommand entrypoints. Each one is defined in its own translation unit
// under src/cmd/. Add a forward declaration here and a CLI11 subcommand
// registration in main() when you add a new one.
int info(const std::string& path, bool json);
int extract(const std::string& path, const std::string& format);
int extract_to(const std::string& path, const std::string& format, std::ostream& os);
int lint(const std::string& path, const LintOptions& opts);
int lint_to(const std::string& path, std::ostream& os, const LintOptions& opts);
std::vector<std::string> lint_selectors();
int diff(const std::string& a,
         const std::string& b,
         const DiffOptions& opts,
         const DiffOutputOptions& out);
int optimize(const std::string& in, const OptimizeOptions& opts, const OptimizeOutputOptions& out);
int merge(const std::vector<std::string>& inputs,
          const MergeOptions& opts,
          const MergeOutputOptions& out);

}  // namespace pptctl::cmd

namespace {

constexpr auto kVersion = "0.6.0";

pptctl::FontTarget parse_font_target(const std::string& s) {
    if (s == "windows") {
        return pptctl::FontTarget::Windows;
    }
    if (s == "macos") {
        return pptctl::FontTarget::MacOS;
    }
    return pptctl::FontTarget::All;
}

int not_implemented(const char* name) {
    pptctl::eprintln("{}: not yet implemented", name);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"pptctl — inspect, lint, diff, optimize, and edit .pptx files"};
    app.set_version_flag("--version", kVersion);
    app.require_subcommand(1);

    // --- info ----------------------------------------------------------------
    std::string info_path;
    bool info_json = false;
    auto* info_cmd = app.add_subcommand("info", "Quick metadata about a .pptx file");
    info_cmd->add_option("file", info_path, "path to .pptx")->required()->check(CLI::ExistingFile);
    info_cmd->add_flag("--json", info_json, "emit machine-readable JSON");
    info_cmd->callback([&]() { std::exit(pptctl::cmd::info(info_path, info_json)); });

    // --- extract -------------------------------------------------------------
    std::string extract_path;
    std::string extract_format = "json";
    auto* extract_cmd =
        app.add_subcommand("extract", "Extract slide text content as a structured dump");
    extract_cmd->add_option("file", extract_path, "path to .pptx")
        ->required()
        ->check(CLI::ExistingFile);
    extract_cmd->add_option("--format", extract_format, "output format: json (default), md, txt")
        ->capture_default_str()
        ->check(CLI::IsMember({"json", "md", "txt"}));
    extract_cmd->callback([&]() { std::exit(pptctl::cmd::extract(extract_path, extract_format)); });

    // --- lint ----------------------------------------------------------------
    std::string lint_path;
    pptctl::LintOptions lint_opts;
    std::string lint_target = "all";
    auto* lint_cmd = app.add_subcommand("lint", "Static analysis for .pptx files");
    lint_cmd->add_option("file", lint_path, "path to .pptx")->required()->check(CLI::ExistingFile);
    lint_cmd->add_flag("--json", lint_opts.json, "emit machine-readable JSON");
    lint_cmd->add_flag("--strict", lint_opts.strict, "exit 2 if any diagnostic is reported");
    lint_cmd
        ->add_option("--max-image-dpi",
                     lint_opts.max_image_dpi,
                     "img-bloat: flag images above this effective DPI (default 300)")
        ->check(CLI::PositiveNumber);
    lint_cmd->add_option("--min-image-bytes",
                         lint_opts.min_image_bytes,
                         "img-bloat: ignore images stored smaller than this (default 65536)");
    lint_cmd
        ->add_option("--min-font-size",
                     lint_opts.min_font_pt,
                     "tiny-text: flag text below this size in pt (default 12)")
        ->check(CLI::PositiveNumber);
    lint_cmd
        ->add_option("--target",
                     lint_target,
                     "font-risk: platform whose bundled fonts count as safe: all (default; "
                     "must be on both Windows and macOS), windows, macos. Office fonts are "
                     "always safe")
        ->check(CLI::IsMember({"all", "windows", "macos"}));
    lint_cmd
        ->add_option("--only",
                     lint_opts.only,
                     "run only these rules: comma-separated rule ids, or the group "
                     "'integrity' (package-integrity rules); repeatable")
        ->delimiter(',')
        ->expected(1)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->check(CLI::IsMember(pptctl::cmd::lint_selectors()));
    lint_cmd->callback([&]() {
        lint_opts.font_target = parse_font_target(lint_target);
        std::exit(pptctl::cmd::lint(lint_path, lint_opts));
    });

    // --- optimize ------------------------------------------------------------
    std::string optimize_path;
    pptctl::OptimizeOptions optimize_opts;
    pptctl::OptimizeOutputOptions optimize_out;
    bool keep_fonts = false;
    bool no_layouts = false;
    bool no_crop = false;
    bool no_images = false;
    bool no_orphans = false;
    auto* optimize_cmd = app.add_subcommand("optimize", "Reduce file size");
    optimize_cmd->add_option("file", optimize_path, "path to .pptx")
        ->required()
        ->check(CLI::ExistingFile);
    auto* optimize_o =
        optimize_cmd->add_option("-o,--output", optimize_out.output, "path to write the result");
    optimize_cmd->add_flag("--in-place", optimize_out.in_place, "overwrite the input file")
        ->excludes(optimize_o);
    optimize_cmd->add_flag(
        "--dry-run", optimize_out.dry_run, "report what would change; write nothing");
    optimize_cmd->add_flag("--json", optimize_out.json, "emit machine-readable JSON");
    optimize_cmd->add_flag("--no-layouts", no_layouts, "keep unused slide layouts and masters");
    optimize_cmd->add_flag("--keep-fonts", keep_fonts, "keep embedded fonts");
    optimize_cmd->add_flag("--no-crop", no_crop, "keep cropped-out image regions");
    optimize_cmd->add_flag("--no-images", no_images, "don't recompress or downsample images");
    optimize_cmd
        ->add_option("--quality", optimize_opts.jpeg_quality, "JPEG re-encode quality (1-100)")
        ->check(CLI::Range(1, 100))
        ->capture_default_str();
    optimize_cmd
        ->add_option("--max-scale",
                     optimize_opts.max_scale,
                     "keep images at up to this many pixels per 96 DPI canvas pixel at their "
                     "largest displayed size")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    optimize_cmd
        ->add_option("--max-dpi",
                     optimize_opts.max_dpi,
                     "keep images at up to this many pixels per inch displayed (overrides "
                     "--max-scale)")
        ->check(CLI::PositiveNumber);
    optimize_cmd->add_flag("--no-orphans", no_orphans, "keep media nothing references");
    optimize_cmd->callback([&]() {
        optimize_opts.layouts = !no_layouts;
        optimize_opts.fonts = !keep_fonts;
        optimize_opts.crop = !no_crop;
        optimize_opts.images = !no_images;
        optimize_opts.orphans = !no_orphans;
        std::exit(pptctl::cmd::optimize(optimize_path, optimize_opts, optimize_out));
    });

    // --- diff ----------------------------------------------------------------
    std::string diff_a;
    std::string diff_b;
    pptctl::DiffOptions diff_opts;
    auto* diff_cmd = app.add_subcommand("diff", "Semantic diff between two .pptx files");
    diff_cmd->add_option("a", diff_a, "path to the old .pptx")
        ->required()
        ->check(CLI::ExistingFile);
    diff_cmd->add_option("b", diff_b, "path to the new .pptx")
        ->required()
        ->check(CLI::ExistingFile);
    diff_cmd
        ->add_option("--match-threshold",
                     diff_opts.match_threshold,
                     "minimum similarity (0-1) to pair edited slides instead of reporting "
                     "remove + add")
        ->check(CLI::Range(0.0, 1.0))
        ->capture_default_str();
    pptctl::DiffOutputOptions diff_out;
    bool diff_color = false;
    bool diff_no_color = false;
    diff_cmd
        ->add_option("--format", diff_out.format, "output format: unified (default), json, html")
        ->capture_default_str()
        ->check(CLI::IsMember({"unified", "json", "html"}));
    diff_cmd->add_flag("--strict-text",
                       diff_opts.strict_text,
                       "also report whitespace-only and field-run (a:fld) text churn");
    diff_cmd
        ->add_option("--geometry-tolerance",
                     diff_opts.geometry_tolerance_emu,
                     "ignore shape moves/resizes at or below this many EMU (9525 = 1 px at "
                     "96 DPI; 0 reports every change)")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    diff_cmd->add_flag("-q,--quiet", diff_out.quiet, "print nothing; exit code only");
    auto* diff_color_flag =
        diff_cmd->add_flag("--color", diff_color, "colour unified output even when not a TTY");
    diff_cmd->add_flag("--no-color", diff_no_color, "never colour unified output")
        ->excludes(diff_color_flag);
    diff_cmd->callback([&]() {
        if (diff_color) {
            diff_out.color = true;
        } else if (diff_no_color) {
            diff_out.color = false;
        }
        std::exit(pptctl::cmd::diff(diff_a, diff_b, diff_opts, diff_out));
    });

    // --- merge ---------------------------------------------------------------
    std::vector<std::string> merge_inputs;
    pptctl::MergeOptions merge_opts;
    pptctl::MergeOutputOptions merge_out;
    auto* merge_cmd = app.add_subcommand("merge", "Merge two or more .pptx files into one");
    merge_cmd->add_option("files", merge_inputs, "paths to the .pptx files, in slide order")
        ->required()
        ->expected(2, CLI::detail::expected_max_vector_size)
        ->check(CLI::ExistingFile);
    merge_cmd->add_option("-o,--output", merge_out.output, "path to write the result");
    merge_cmd
        ->add_option("--theme",
                     merge_opts.theme,
                     "restyle every slide onto deck N's masters and layouts (1-based); by "
                     "default each deck keeps its own formatting")
        ->check(CLI::PositiveNumber);
    merge_cmd->add_flag("--dry-run", merge_out.dry_run, "report what would change; write nothing");
    merge_cmd->add_flag("--json", merge_out.json, "emit machine-readable JSON");
    merge_cmd->callback(
        [&]() { std::exit(pptctl::cmd::merge(merge_inputs, merge_opts, merge_out)); });

    // --- placeholders for future subcommands ---------------------------------
    // Each of these is a TODO in ROADMAP.md. They're listed here so `--help`
    // shows the full planned surface area to users.
    app.add_subcommand("fill", "Template-fill a .pptx from JSON data (not yet implemented)")
        ->callback([] { std::exit(not_implemented("fill")); });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
