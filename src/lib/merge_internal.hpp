// merge_internal.hpp — the merge pipeline's building blocks, shared between
// src/lib/merge*.cpp and the tests. Not part of the public API.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <pptctl/package.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl::detail {

// The output package being assembled: the base package plus the parts added to
// it so far. Owns the PartChanges handed to Package::write.
class MergeTarget {
 public:
    explicit MergeTarget(const Package& base);

    // A part name no base or added part has, in `source_name`'s family: same
    // directory, stem (trailing digits dropped) and extension, numbered one
    // past the highest index already used. "ppt/slides/slide1.xml" becomes
    // "ppt/slides/slide4.xml" when slides 1-3 exist. Names compare
    // case-insensitively, as OPC part names do. The name is reserved.
    [[nodiscard]] std::string allocate(std::string_view source_name);

    // Add a part (its name from allocate) with its content type, which is
    // declared through a Default when the extension is new to the package,
    // and through an Override when the Default for it says something else.
    // An empty content type declares nothing.
    void add_part(const std::string& name,
                  std::vector<std::byte> bytes,
                  const std::string& content_type);

    // Replace the bytes of an existing part, base or added; its content type
    // stays as it was.
    void replace_part(const std::string& name, std::vector<std::byte> bytes);

    // A part's current bytes: replaced or added, else the base's.
    [[nodiscard]] std::vector<std::byte> read_part(const std::string& name) const;

    // A part's content type: as added, else the base's.
    [[nodiscard]] std::string content_type(const std::string& name) const;

    // Every part in the output: the base's, then the added ones.
    [[nodiscard]] std::vector<std::string> part_names() const;

    // The added parts, in the order they were added.
    [[nodiscard]] const std::vector<std::string>& added() const noexcept {
        return added_;
    }

    // The changes to write: the added and replaced parts, plus
    // `[Content_Types].xml` rewritten when anything needed declaring.
    [[nodiscard]] PartChanges changes() const;

    [[nodiscard]] const Package& base() const noexcept {
        return *base_;
    }

 private:
    const Package* base_;
    std::set<std::string> taken_;                  // lowercased base + allocated names
    std::map<std::string, std::string> defaults_;  // extension (lowercase) -> content type
    std::map<std::string, std::string> added_defaults_;
    std::map<std::string, std::string> added_overrides_;
    std::map<std::string, std::string> added_types_;  // added part -> content type
    std::vector<std::string> added_;                  // added parts, in add order
    PartChanges changes_;
};

// Consulted before a part is copied, with the type of the relationship that
// reached it ("" for the part import() was called with): the name of an
// existing output part that stands in for it (a base layout under --theme, an
// identical media part), or nullopt to copy it. A part resolved this way is
// not copied and its own relationships are not followed.
using PartResolver = std::function<std::optional<std::string>(std::string_view source_part,
                                                              std::string_view rel_type)>;

// The output's media parts (`ppt/media/`, where Office and python-pptx store
// images, audio and video), for de-duplication by content (#137). Holds the
// base's media from the start and picks up imported media as the target
// gains them.
class MediaIndex {
 public:
    explicit MediaIndex(const MergeTarget& target);

    // An output media part with the same bytes as `part` of `source`, reached
    // through a relationship of `rel_type`, or nullopt. Parts outside
    // `ppt/media/` and parts reached through an exclusively-owned type
    // (is_exclusively_owned) never match. Candidates are narrowed by size,
    // then content hash, then compared byte for byte.
    [[nodiscard]] std::optional<std::string> find(const Package& source,
                                                  std::string_view part,
                                                  std::string_view rel_type);

 private:
    void add(const std::string& name, std::uint64_t size);
    void catch_up();  // index media the target added since the last call

    const MergeTarget* target_;
    std::size_t seen_ = 0;  // how many of target_->added() are indexed
    std::map<std::uint64_t, std::vector<std::string>> by_size_;
    std::map<std::string, std::uint64_t> hashes_;  // computed on demand
};

// Copies parts from one source package into a MergeTarget. Remembers where
// every source part went, so a part reached from several slides (a layout,
// a master, a slide another slide links to) is copied once.
class PartImporter {
 public:
    PartImporter(const Package& source, MergeTarget& target, PartResolver resolve = {});

    // Copy `part` and every part reachable from it through internal
    // relationships, each with its `.rels` re-pointed at the copies. Part
    // bodies are copied byte for byte: relationship ids are part-scoped and
    // kept. Returns the part's name in the output, or "" if `part` is missing
    // from the source.
    std::string import(const std::string& part);

    // Source part name -> output part name, for everything imported or resolved.
    [[nodiscard]] const std::map<std::string, std::string>& mapping() const noexcept {
        return mapping_;
    }
    // The source parts copied (not resolved), in copy order; `.rels` not included.
    [[nodiscard]] const std::vector<std::string>& copied() const noexcept {
        return copied_;
    }
    // Problems found on the way (a relationship to a missing part, a part
    // with no content type), for the merge report.
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept {
        return warnings_;
    }

 private:
    const Package* source_;
    MergeTarget* target_;
    PartResolver resolve_;
    std::map<std::string, std::string> mapping_;
    std::vector<std::string> copied_;
    std::vector<std::string> warnings_;
};

// The parts a slide master brings (its layouts, theme, media, ...), walked
// from the master through internal relationships in `.rels` order, with a
// signature that two masters share exactly when their closures have the same
// content types and part hashes and are wired the same way. Part names don't
// enter it, so slideMaster2.xml in one deck matches slideMaster1.xml in another.
struct MasterClosure {
    std::vector<std::string> parts;  // walk order, the master first
    std::string signature;
};

[[nodiscard]] MasterClosure master_closure(const Package& pkg, const std::string& master);

// The slide masters already in the output, for collapsing identical ones (#139).
// Identity is byte equality of the whole closure: a signature match is
// confirmed part by part. Near-identical masters (whitespace, ids, a
// re-saved theme) don't match.
class MasterIndex {
 public:
    // Index the base's slide masters (those its presentation part relates).
    explicit MasterIndex(const Package& base);

    // If `source`'s `master` is identical to one in the output: each part of
    // its closure (source name) -> the matching output part. Empty if not.
    [[nodiscard]] std::map<std::string, std::string> match(const Package& source,
                                                           const std::string& master) const;

    // Index `master`, imported from `source`; `mapping` is the importer's
    // source part -> output part.
    void add(const Package& source,
             const std::string& master,
             const std::map<std::string, std::string>& mapping);

 private:
    struct Entry {
        const Package* pkg;               // where the closure's bytes are read from
        MasterClosure closure;            // part names in *pkg
        std::vector<std::string> output;  // the same parts' output names
    };
    std::vector<Entry> entries_;
};

// How a foreign slide layout was placed on the --theme deck's layouts (#138).
enum class LayoutMatchKind {
    type,      // same sldLayout@type (not "cust", which says nothing)
    name,      // same layout name (p:cSld@name)
    fallback,  // neither: the "Title and Content" (obj) layout, else the first
};

struct LayoutMatch {
    std::string part;  // the --theme deck's layout
    LayoutMatchKind kind = LayoutMatchKind::fallback;
    std::string source_name;  // the foreign layout's name
    std::string target_name;  // the chosen layout's name
};

// The --theme deck's slide layouts, in master then sldLayoutIdLst order, for
// restyling foreign slides onto them.
class LayoutMatcher {
 public:
    explicit LayoutMatcher(const Package& base);

    // Where `layout` of `source` goes: the first base layout of the same type,
    // else of the same name, else the fallback. nullopt if the base has no
    // layouts at all.
    [[nodiscard]] std::optional<LayoutMatch> match(const Package& source,
                                                   const std::string& layout) const;

 private:
    struct Layout {
        std::string part;
        std::string type;
        std::string name;
    };
    std::vector<Layout> layouts_;
};

// Comment authors (#159). Comments come with their slide's closure, but the
// authors they name live in a presentation-level part a slide never reaches:
// legacy comments (p:cm authorId) in commentAuthors.xml, modern comments
// (p188:cm authorId, a GUID) in authors.xml. This carries the authors over
// and re-points the imported comments at them.
class CommentAuthors {
 public:
    explicit CommentAuthors(MergeTarget& target);

    // Point the comment parts imported from `source` (their output names) at
    // the output's authors. A legacy author is matched by name and initials,
    // else added with the next free id; each imported comment gets the next
    // idx past its author's lastIdx, and reply links (p15:parentCm) follow.
    // Modern authors are added by GUID when missing; their comments need no
    // rewrite. Problems (an author the source doesn't list) go to `warnings`.
    void import(const Package& source,
                const std::vector<std::string>& comment_parts,
                std::vector<std::string>& warnings);

    // Write the author lists that changed, creating each part with its
    // presentation relationship and content type when the base had none.
    void finish();

 private:
    struct Legacy;
    struct Modern;
    MergeTarget* target_;
    std::shared_ptr<Legacy> legacy_;
    std::shared_ptr<Modern> modern_;
};

// Add a relationship of `type` from the output's presentation part to
// `target_part`, with a fresh rId; returns the rId.
std::string add_presentation_relationship(MergeTarget& target,
                                          std::string_view type,
                                          const std::string& target_part);

// One input deck as concatenate() sees it.
struct ConcatDeck {
    const Package* source = nullptr;
    std::string path;  // names the section of a deck that has none
    // Source part -> output part for an imported deck; nullptr for the base,
    // whose slides are already in the output.
    const std::map<std::string, std::string>* imported = nullptr;
    std::vector<std::string> masters;  // slide masters imported from this deck (output names)

    // Filled in by concatenate():
    std::size_t slides = 0;             // slides this deck has in the output's slide list
    std::vector<std::string> warnings;  // e.g. a slide size that differs from the base's
};

// Assemble the output's presentation-level parts (#136) after every deck's
// slides are imported: rebuild p:sldIdLst in deck order (the base's entries
// kept, imported slides with fresh ids and relationships), register imported
// slide masters with their master and layout ids renumbered into one unique
// id space, list the notes master in p:notesMasterIdLst (`imported_notes_master`
// when non-empty, else the base's if it has one but doesn't list it),
// carry sections over (a deck without sections gets one named after its
// file), warn about decks whose slide size differs from the base's, and
// update the Slides and Notes counts in docProps/app.xml.
void concatenate(MergeTarget& target,
                 std::span<ConcatDeck> decks,
                 const std::string& imported_notes_master);

// Add <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>, the value PowerPoint
// writes, to each of the output's notes slides `notes` that lacks it:
// LibreOffice and ONLYOFFICE leave it out and Keynote then refuses the whole
// deck (#166). Notes slides that have it stay byte for byte. Returns how many
// were changed.
std::size_t add_notes_clr_map_ovr(MergeTarget& target, std::span<const std::string> notes);

}  // namespace pptctl::detail
