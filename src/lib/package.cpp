// pptctl/package.cpp — real implementation backed by minizip-ng.

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <map>
#include <pptctl/package.hpp>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// Extension of a part name (without the dot), lowercased. Empty if none.
std::string extension_of(std::string_view name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const auto slash = name.find_last_of('/');
    if (slash != std::string_view::npos && dot < slash) {
        return {};
    }
    return to_lower(name.substr(dot + 1));
}

// Directory portion of a part name, including the trailing slash. "" if none.
std::string dir_of(std::string_view part) {
    const auto slash = part.find_last_of('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    return std::string(part.substr(0, slash + 1));
}

// The `_rels/*.rels` path that declares relationships for `part`.
// An empty part denotes the package root → "_rels/.rels".
std::string rels_path_for(std::string_view part) {
    const auto slash = part.find_last_of('/');
    const std::string dir =
        (slash == std::string_view::npos) ? "" : std::string(part.substr(0, slash + 1));
    const std::string name =
        (slash == std::string_view::npos) ? std::string(part) : std::string(part.substr(slash + 1));
    return dir + "_rels/" + name + ".rels";
}

// Collapse "." and ".." segments in a slash-separated path.
std::string normalize_path(std::string_view path) {
    std::vector<std::string_view> stack;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = (slash == std::string_view::npos) ? path.size() : slash;
        const std::string_view seg = path.substr(start, end - start);
        if (seg.empty() || seg == ".") {
            // skip
        } else if (seg == "..") {
            if (!stack.empty() && stack.back() != "..") {
                stack.pop_back();
            } else {
                stack.push_back(seg);
            }
        } else {
            stack.push_back(seg);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string out;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i != 0) {
            out += '/';
        }
        out += stack[i];
    }
    return out;
}

// Resolve a relationship Target against the source part's directory.
// Absolute targets (leading '/') are package-root relative.
std::string resolve_target(std::string_view base_dir, std::string_view target) {
    if (!target.empty() && target.front() == '/') {
        return normalize_path(target.substr(1));
    }
    return normalize_path(std::string(base_dir) + std::string(target));
}

}  // namespace

struct Package::Impl {
    void* reader = nullptr;
    std::string path;
    std::vector<std::string> parts;

    // Lazily-populated caches (see lazy-parsing note in the header).
    bool ct_loaded = false;
    std::map<std::string, std::string> ct_overrides;  // "/ppt/.../x.xml" -> content type
    std::map<std::string, std::string> ct_defaults;   // lowercased extension -> content type
    std::map<std::string, std::vector<Relationship>> rels_cache;  // source part -> relationships

    ~Impl() {
        close();
    }

    void close() {
        if (reader != nullptr) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            reader = nullptr;
        }
        ct_loaded = false;
        ct_overrides.clear();
        ct_defaults.clear();
        rels_cache.clear();
    }

    [[nodiscard]] bool is_open() const noexcept {
        return reader != nullptr;
    }
};

Package::Package() : impl_(new Impl()) {}
Package::~Package() {
    delete impl_;
}

Package::Package(Package&& other) noexcept : impl_(other.impl_) {
    other.impl_ = new Impl();
}

Package& Package::operator=(Package&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = new Impl();
    }
    return *this;
}

bool Package::open(const std::string& path) {
    impl_->close();
    impl_->path = path;
    impl_->parts.clear();

    impl_->reader = mz_zip_reader_create();
    if (impl_->reader == nullptr) {
        return false;
    }
    if (mz_zip_reader_open_file(impl_->reader, path.c_str()) != MZ_OK) {
        impl_->close();
        return false;
    }

    // Enumerate entries to build the part list.
    int err = mz_zip_reader_goto_first_entry(impl_->reader);
    while (err == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(impl_->reader, &info) == MZ_OK && info != nullptr &&
            info->filename != nullptr) {
            // Skip directory entries.
            const std::size_t len = std::strlen(info->filename);
            if (len > 0 && info->filename[len - 1] != '/') {
                impl_->parts.emplace_back(info->filename);
            }
        }
        err = mz_zip_reader_goto_next_entry(impl_->reader);
    }
    return true;
}

bool Package::is_open() const noexcept {
    return impl_->is_open();
}

std::size_t Package::part_count() const noexcept {
    return impl_->parts.size();
}

std::vector<std::string> Package::part_names() const {
    return impl_->parts;
}

std::vector<std::byte> Package::read_part(std::string_view name) const {
    if (impl_->reader == nullptr) {
        return {};
    }

    // minizip-ng wants a C-string for locate.
    const std::string name_str(name);
    if (mz_zip_reader_locate_entry(impl_->reader, name_str.c_str(), 0) != MZ_OK) {
        return {};
    }

    const int32_t buf_len = mz_zip_reader_entry_save_buffer_length(impl_->reader);
    if (buf_len < 0) {
        return {};
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(buf_len));
    if (buf_len > 0) {
        if (mz_zip_reader_entry_save_buffer(
                impl_->reader, bytes.data(), static_cast<int32_t>(bytes.size())) != MZ_OK) {
            return {};
        }
    }
    return bytes;
}

std::string Package::content_type(std::string_view part_name) const {
    if (!impl_->is_open()) {
        return {};
    }
    if (!impl_->ct_loaded) {
        const auto bytes = read_part("[Content_Types].xml");
        pugi::xml_document doc;
        if (!bytes.empty() &&
            doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
            for (auto node : doc.child("Types").children()) {
                const std::string_view name = node.name();
                if (name == "Default") {
                    const std::string ext = to_lower(node.attribute("Extension").as_string(""));
                    if (!ext.empty()) {
                        impl_->ct_defaults[ext] = node.attribute("ContentType").as_string("");
                    }
                } else if (name == "Override") {
                    const std::string part = node.attribute("PartName").as_string("");
                    if (!part.empty()) {
                        impl_->ct_overrides[part] = node.attribute("ContentType").as_string("");
                    }
                }
            }
        }
        impl_->ct_loaded = true;  // cache even when the file is absent, to avoid re-reading
    }

    // Override match is keyed by absolute part path ("/ppt/...").
    std::string key(part_name);
    if (key.empty() || key.front() != '/') {
        key.insert(key.begin(), '/');
    }
    const auto over = impl_->ct_overrides.find(key);
    if (over != impl_->ct_overrides.end()) {
        return over->second;
    }
    const std::string ext = extension_of(part_name);
    if (!ext.empty()) {
        const auto def = impl_->ct_defaults.find(ext);
        if (def != impl_->ct_defaults.end()) {
            return def->second;
        }
    }
    return {};
}

std::vector<Relationship> Package::relationships(std::string_view source_part) const {
    if (!impl_->is_open()) {
        return {};
    }
    std::string key(source_part);
    if (!key.empty() && key.front() == '/') {
        key.erase(key.begin());
    }
    const auto cached = impl_->rels_cache.find(key);
    if (cached != impl_->rels_cache.end()) {
        return cached->second;
    }

    std::vector<Relationship> rels;
    const auto bytes = read_part(rels_path_for(key));
    pugi::xml_document doc;
    if (!bytes.empty() &&
        doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
        const std::string base = dir_of(key);
        for (auto node : doc.child("Relationships").children("Relationship")) {
            Relationship rel;
            rel.id = node.attribute("Id").as_string("");
            rel.type = node.attribute("Type").as_string("");
            rel.external =
                std::string_view(node.attribute("TargetMode").as_string("")) == "External";
            const std::string_view target = node.attribute("Target").as_string("");
            rel.target = rel.external ? std::string(target) : resolve_target(base, target);
            rels.push_back(std::move(rel));
        }
    }
    const auto inserted = impl_->rels_cache.emplace(std::move(key), std::move(rels));
    return inserted.first->second;
}

std::string Package::resolve(std::string_view source_part, std::string_view rel_id) const {
    for (const auto& rel : relationships(source_part)) {
        if (rel.id == rel_id) {
            return rel.external ? std::string{} : rel.target;
        }
    }
    return {};
}

}  // namespace pptctl
