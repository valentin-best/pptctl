// pptctl — a CLI for .pptx files.
//
// This file is just the dispatch layer. All actual logic lives in
// src/cmd/*.cpp and the internal library in src/lib/.

#include <CLI/CLI.hpp>
#include <ostream>
#include <pptctl/lint.hpp>
#include <pptctl/print.hpp>
#include <string>

namespace pptctl::cmd {

// Subcommand entrypoints. Each one is defined in its own translation unit
// under src/cmd/. Add a forward declaration here and a CLI11 subcommand
// registration in main() when you add a new one.
int info(const std::string& path, bool json);
int extract(const std::string& path, const std::string& format);
int extract_to(const std::string& path, const std::string& format, std::ostream& os);
int lint(const std::string& path, const LintOptions& opts);
int lint_to(const std::string& path, std::ostream& os, const LintOptions& opts);

}  // namespace pptctl::cmd

namespace {

constexpr auto kVersion = "0.0.1-dev";

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
    lint_cmd->callback([&]() {
        lint_opts.font_target = parse_font_target(lint_target);
        std::exit(pptctl::cmd::lint(lint_path, lint_opts));
    });

    // --- placeholders for future subcommands ---------------------------------
    // Each of these is a TODO in ROADMAP.md. They're listed here so `--help`
    // shows the full planned surface area to users.
    app.add_subcommand("optimize", "Reduce file size (not yet implemented)")->callback([] {
        std::exit(not_implemented("optimize"));
    });
    app.add_subcommand("diff", "Semantic diff between two .pptx files (not yet implemented)")
        ->callback([] { std::exit(not_implemented("diff")); });
    app.add_subcommand("merge", "Merge multiple .pptx files (not yet implemented)")->callback([] {
        std::exit(not_implemented("merge"));
    });
    app.add_subcommand("fill", "Template-fill a .pptx from JSON data (not yet implemented)")
        ->callback([] { std::exit(not_implemented("fill")); });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
