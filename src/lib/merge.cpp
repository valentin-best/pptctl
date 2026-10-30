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
constexpr std::string_view kNotesMasterType =
    "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml";
constexpr std::string_view kNotesMasterRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster";
constexpr std::string_view kOfficeDocumentRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";

// The target of `source`'s first internal relationship of `type`, or "".
std::string target_of_type(const Package& pkg, std::string_view source, std::string_view type) {
    for (const auto& rel : pkg.relationships(source)) {
        if (!rel.external && rel.type == type) {
            return rel.target;
        }
    }
    return {};
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

    detail::MergeTarget target(packages[base]);

    // A presentation has one notes master: imported notes slides use the
    // base's, or the first one imported when the base has none.
    std::string notes_master = target_of_type(
        packages[base], target_of_type(packages[base], "", kOfficeDocumentRel), kNotesMasterRel);
    std::string imported_notes_master;

    std::vector<detail::PartImporter> importers;
    importers.reserve(packages.size());
    std::vector<detail::ConcatDeck> decks(packages.size());
    for (std::size_t i = 0; i < packages.size(); ++i) {
        auto& deck = decks[i];
        deck.source = &packages[i];
        deck.path = in_paths[i];
        if (i == base) {
            continue;
        }
        const auto& source = packages[i];
        auto& importer = importers.emplace_back(
            source,
            target,
            [&notes_master, src = &source](std::string_view part) -> std::optional<std::string> {
                if (!notes_master.empty() && src->content_type(part) == kNotesMasterType) {
                    return notes_master;
                }
                return std::nullopt;
            });
        for (const auto& slide : slide_parts[i]) {
            (void)importer.import(slide);
        }
        auto& input = report.inputs[i];
        input.parts_added = importer.copied().size();
        for (const auto& part : importer.copied()) {
            const auto type = source.content_type(part);
            if (type == kSlideMasterType) {
                ++input.masters_imported;
                deck.masters.push_back(importer.mapping().at(part));
            } else if (type == kNotesMasterType && notes_master.empty()) {
                notes_master = imported_notes_master = importer.mapping().at(part);
            }
        }
        input.warnings = importer.warnings();
        deck.imported = &importer.mapping();
    }

    detail::concatenate(target, decks, imported_notes_master);
    for (std::size_t i = 0; i < decks.size(); ++i) {
        auto& input = report.inputs[i];
        input.slides = decks[i].slides;
        input.warnings.insert(
            input.warnings.end(), decks[i].warnings.begin(), decks[i].warnings.end());
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
