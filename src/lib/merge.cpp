// merge.cpp — the merge pipeline.

#include <filesystem>
#include <format>
#include <pptctl/merge.hpp>
#include <pptctl/package.hpp>
#include <pptctl/presentation.hpp>
#include <random>
#include <system_error>
#include <utility>

#include "merge_internal.hpp"

namespace pptctl {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSlideMasterType =
    "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml";

// Copy `slides` of `input`, with everything they reach, into `target`. The
// slides aren't added to the output's slide list yet (#136).
void import_slides(const Package& input,
                   const std::vector<std::string>& slides,
                   detail::MergeTarget& target,
                   MergeInputReport& report) {
    detail::PartImporter importer(input, target);
    std::size_t imported = 0;
    for (const auto& slide : slides) {
        if (!importer.import(slide).empty()) {
            ++imported;
        }
    }
    report.parts_added = importer.copied().size();
    for (const auto& part : importer.copied()) {
        if (input.content_type(part) == kSlideMasterType) {
            ++report.masters_imported;
        }
    }
    report.warnings = importer.warnings();
    report.warnings.push_back(std::format(
        "{} imported but not added to the slide list yet: the output shows only the base "
        "deck's slides",
        imported == 1 ? "1 slide" : std::format("{} slides", imported)));
}

// Write `base` with `changes` to a throwaway file and return its size, for a
// dry run. nullopt if the write fails.
std::optional<std::uint64_t> measure(Package& base, const PartChanges& changes) {
    const auto tag = std::to_string(std::random_device{}());
    const auto path = fs::temp_directory_path() / fs::path("pptctl-merge-" + tag + ".pptx");
    if (!base.write(path.string(), changes)) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    fs::remove(path, ec);
    return size;
}

}  // namespace

std::optional<MergeReport> merge(std::span<const std::string> in_paths,
                                 const std::string& out_path,
                                 const MergeOptions& opts,
                                 std::string& error) {
    if (in_paths.size() < 2) {
        error = "merge needs at least two input decks";
        return std::nullopt;
    }
    if (opts.theme > in_paths.size()) {
        error = std::format(
            "--theme={} is out of range: there are {} input decks", opts.theme, in_paths.size());
        return std::nullopt;
    }

    // Every input stays open for the whole merge: the base is written from,
    // the others are read from while their parts are imported.
    std::vector<Package> packages(in_paths.size());
    std::vector<std::vector<std::string>> slide_parts;
    MergeReport report;
    for (std::size_t i = 0; i < in_paths.size(); ++i) {
        const auto& path = in_paths[i];
        auto& pkg = packages[i];
        if (!pkg.open(path)) {
            error = std::format("cannot open '{}'", path);
            return std::nullopt;
        }
        Presentation pres(pkg);
        if (!pres.load()) {
            error = std::format("'{}' is not a presentation", path);
            return std::nullopt;
        }
        MergeInputReport input;
        input.path = path;
        std::error_code ec;
        input.bytes = fs::file_size(path, ec);
        slide_parts.push_back(pres.slide_parts());
        report.inputs.push_back(std::move(input));
    }

    const std::size_t base = opts.theme == 0 ? 0 : opts.theme - 1;
    report.inputs[base].base = true;
    report.inputs[base].slides = slide_parts[base].size();

    detail::MergeTarget target(packages[base]);
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (i != base) {
            import_slides(packages[i], slide_parts[i], target, report.inputs[i]);
        }
    }
    const auto changes = target.changes();
    for (const auto& input : report.inputs) {
        report.slides += input.slides;
    }

    if (out_path.empty()) {
        const auto size = measure(packages[base], changes);
        if (!size) {
            error = "cannot write a temporary package to measure the result";
            return std::nullopt;
        }
        report.output_bytes = *size;
        return report;
    }
    if (!packages[base].write(out_path, changes)) {
        error = std::format("cannot write '{}'", out_path);
        return std::nullopt;
    }
    std::error_code ec;
    report.output_bytes = fs::file_size(out_path, ec);
    return report;
}

}  // namespace pptctl
