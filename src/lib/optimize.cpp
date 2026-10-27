// optimize.cpp — the optimize pass pipeline and removal bookkeeping.

#include <algorithm>
#include <filesystem>
#include <format>
#include <pptctl/optimize.hpp>
#include <pugixml.hpp>
#include <random>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#include "optimize_internal.hpp"

namespace pptctl {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kContentTypes = "[Content_Types].xml";

// The part a `.rels` part belongs to: the inverse of rels_part_for.
// "ppt/slides/_rels/slide1.xml.rels" -> "ppt/slides/slide1.xml", "_rels/.rels" -> "".
std::string rels_owner(std::string_view rels) {
    constexpr std::string_view kDir = "_rels/";
    constexpr std::string_view kExt = ".rels";
    const auto dir = rels.rfind(kDir);
    if (dir == std::string_view::npos || !rels.ends_with(kExt)) {
        return {};
    }
    const auto name = rels.substr(dir + kDir.size(), rels.size() - dir - kDir.size() - kExt.size());
    return std::string(rels.substr(0, dir)) + std::string(name);
}

bool is_rels(std::string_view part) {
    return part.ends_with(".rels") &&
           (part.starts_with("_rels/") || part.find("/_rels/") != std::string_view::npos);
}

// True if `xml` (a [Content_Types].xml) has an Override for any of `parts`.
bool has_override_for(std::span<const std::byte> xml, const std::set<std::string>& parts) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) {
        return false;
    }
    const auto overrides = doc.child("Types").children("Override");
    return std::any_of(overrides.begin(), overrides.end(), [&](const pugi::xml_node& node) {
        const std::string_view name = node.attribute("PartName").as_string("");
        return name.starts_with('/') && parts.contains(std::string(name.substr(1)));
    });
}

// Intermediate packages between passes, in a private temp directory created
// on first use and removed with everything in it on scope exit.
class Scratch {
 public:
    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;
    ~Scratch() {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }

    // A fresh file path, or empty if the directory can't be created.
    std::string next() {
        if (dir_.empty()) {
            const auto tag = std::to_string(std::random_device{}());
            const auto dir = fs::temp_directory_path() / fs::path("pptctl-optimize-" + tag);
            std::error_code ec;
            if (!fs::create_directories(dir, ec)) {
                return {};
            }
            dir_ = dir;
        }
        return (dir_ / fs::path(std::format("step{}.pptx", ++count_))).string();
    }

 private:
    fs::path dir_;
    int count_ = 0;
};

}  // namespace

const std::vector<RegisteredPass>& optimize_passes() {
    static const std::vector<RegisteredPass> passes{
        {"layouts", &OptimizeOptions::layouts, detail::optimize_layouts},
        {"fonts", &OptimizeOptions::fonts, detail::optimize_fonts},
        {"crop", &OptimizeOptions::crop, detail::optimize_crop},
        {"images", &OptimizeOptions::images, detail::optimize_images},
        {"orphans", &OptimizeOptions::orphans, detail::optimize_orphans},
    };
    return passes;
}

PartChanges complete_removals(const Package& package, const PartGraph& graph, PartChanges changes) {
    if (changes.removed.empty()) {
        return changes;
    }
    const std::set<std::string> removed = changes.removed;
    const auto current = [&](const std::string& part) {
        const auto it = changes.replaced.find(part);
        return it != changes.replaced.end() ? it->second : package.read_part(part);
    };

    // A removed part's own .rels goes with it.
    for (const auto& part : removed) {
        const auto rels = rels_part_for(part);
        if (package.part_size(rels)) {
            changes.removed.insert(rels);
        }
    }

    // Every surviving source that points at a removed part, plus any .rels the
    // pass rewrote itself (it may name a removed part the graph doesn't know).
    std::set<std::string> sources;
    for (const auto& part : removed) {
        for (const auto& ref : graph.references_to(part)) {
            sources.insert(ref.source);
        }
    }
    for (const auto& [part, bytes] : changes.replaced) {
        if (is_rels(part)) {
            sources.insert(rels_owner(part));
        }
    }
    for (const auto& source : sources) {
        const auto rels = rels_part_for(source);
        if (removed.contains(source) || changes.removed.contains(rels)) {
            continue;
        }
        changes.replaced[rels] = rewrite_rels(current(rels), source, removed);
    }

    const std::string types(kContentTypes);
    const auto types_xml = current(types);
    if (has_override_for(types_xml, changes.removed)) {
        changes.replaced[types] = rewrite_content_types(types_xml, changes.removed, {});
    }
    return changes;
}

std::optional<OptimizeReport> optimize(const std::string& in_path,
                                       const std::string& out_path,
                                       const OptimizeOptions& opts,
                                       std::string& error,
                                       std::span<const RegisteredPass> passes) {
    // Declared before `current` so the intermediate files outlive the reader
    // holding the last of them open (Windows can't delete an open file).
    Scratch scratch;
    Package current;
    if (!current.open(in_path)) {
        error = std::format("cannot open '{}'", in_path);
        return std::nullopt;
    }
    OptimizeReport report;
    std::error_code ec;
    report.input_bytes = fs::file_size(in_path, ec);
    std::string current_path = in_path;

    for (const auto& pass : passes) {
        if (!(opts.*pass.enabled)) {
            continue;
        }
        const auto graph = PartGraph::build(current);
        auto result = pass.run(current, graph, opts);
        report.passes.push_back({pass.id, std::move(result.actions)});
        if (result.changes.replaced.empty() && result.changes.removed.empty()) {
            continue;
        }
        const auto next = scratch.next();
        Package reopened;
        if (next.empty() ||
            !current.write(next, complete_removals(current, graph, std::move(result.changes))) ||
            !reopened.open(next)) {
            error = std::format("the {} pass could not write an intermediate package", pass.id);
            return std::nullopt;
        }
        current = std::move(reopened);
        current_path = next;
    }

    if (out_path.empty()) {
        report.output_bytes = fs::file_size(current_path, ec);
        return report;
    }
    if (!current.write(out_path, {})) {
        error = std::format("cannot write '{}'", out_path);
        return std::nullopt;
    }
    report.output_bytes = fs::file_size(out_path, ec);
    return report;
}

std::optional<OptimizeReport> optimize(const std::string& in_path,
                                       const std::string& out_path,
                                       const OptimizeOptions& opts,
                                       std::string& error) {
    return optimize(in_path, out_path, opts, error, optimize_passes());
}

}  // namespace pptctl
