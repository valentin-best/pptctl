// pptctl/package.cpp — real implementation backed by minizip-ng.

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <pptctl/package.hpp>
#include <pugixml.hpp>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pptctl {

namespace {

// "12.5 MiB"-style size for limit messages.
std::string mib(std::uint64_t bytes) {
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// nullopt if `info` keeps the package within the decompression limits
// (package.hpp), adding its declared size to `total`; else why not.
std::optional<std::string> check_limits(const mz_zip_file& info, std::uint64_t& total) {
    const auto size = static_cast<std::uint64_t>(std::max<std::int64_t>(info.uncompressed_size, 0));
    const auto packed = static_cast<std::uint64_t>(std::max<std::int64_t>(info.compressed_size, 0));
    if (size >= kInflateRatioMinBytes && size > packed * kMaxInflateRatio) {
        return std::format(
            "part \"{}\" would inflate from {} to {} (over {}:1), more than any real deck; "
            "refusing it as a likely decompression bomb",
            info.filename,
            packed < 1024 ? std::format("{} B", packed) : mib(packed),
            mib(size),
            kMaxInflateRatio);
    }
    total += size;
    if (total > kMaxPackageBytes) {
        return std::format("its parts would inflate to more than {} in all; refusing it",
                           mib(kMaxPackageBytes));
    }
    return std::nullopt;
}

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

// Parse an XML part keeping its declaration, so re-serializing preserves it.
bool load_xml(pugi::xml_document& doc, std::span<const std::byte> xml) {
    constexpr unsigned flags =
        pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi;
    return static_cast<bool>(doc.load_buffer(xml.data(), xml.size(), flags, pugi::encoding_utf8));
}

// Serialize without indentation or added newlines, the way Office writes parts.
std::vector<std::byte> save_xml(const pugi::xml_document& doc) {
    struct BytesWriter : pugi::xml_writer {
        std::vector<std::byte> out;
        void write(const void* data, std::size_t size) override {
            const auto* p = static_cast<const std::byte*>(data);
            out.insert(out.end(), p, p + size);
        }
    } writer;
    doc.save(writer, "", pugi::format_raw, pugi::encoding_utf8);
    return std::move(writer.out);
}

// RAII wrapper over a minizip-ng writer: closed and deleted on scope exit.
struct ZipWriter {
    void* handle = mz_zip_writer_create();

    ~ZipWriter() {
        if (handle != nullptr) {
            mz_zip_writer_close(handle);
            mz_zip_writer_delete(&handle);
        }
    }
    ZipWriter() = default;
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
};

// Add one entry with the given metadata and (uncompressed) bytes, deflated.
bool write_entry(void* writer, mz_zip_file info, std::span<const std::byte> bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    info.compressed_size = 0;
    // The real size up front: minizip's zip64 auto-detection treats an unknown
    // (0) size as "might be huge" and emits zip64 placeholders in the local header.
    info.uncompressed_size = static_cast<int64_t>(bytes.size());
    info.crc = 0;
    info.flag &= static_cast<uint16_t>(~MZ_ZIP_FLAG_ENCRYPTED);
    info.extrafield = nullptr;
    info.extrafield_size = 0;
    // minizip's buffer API takes a non-const pointer but only reads from it.
    void* buf =
        const_cast<std::byte*>(bytes.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    static std::byte empty{};
    if (bytes.empty()) {
        buf = &empty;
    }
    return mz_zip_writer_add_buffer(writer, buf, static_cast<int32_t>(bytes.size()), &info) ==
           MZ_OK;
}

// 1980-01-01 00:00 local time, the earliest DOS date: what Office on Windows
// stamps every entry with.
std::time_t dos_epoch() {
    std::tm tm{};
    tm.tm_year = 80;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
        return true;
    }
    ec.clear();
    return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
}

}  // namespace

std::string rels_part_for(std::string_view part) {
    const auto slash = part.find_last_of('/');
    const std::string dir =
        (slash == std::string_view::npos) ? "" : std::string(part.substr(0, slash + 1));
    const std::string name =
        (slash == std::string_view::npos) ? std::string(part) : std::string(part.substr(slash + 1));
    return dir + "_rels/" + name + ".rels";
}

std::vector<std::byte> rewrite_content_types(
    std::span<const std::byte> xml,
    const std::set<std::string>& removed_parts,
    const std::map<std::string, std::string>& added_overrides,
    const std::map<std::string, std::string>& added_defaults) {
    pugi::xml_document doc;
    if (!load_xml(doc, xml)) {
        return {};
    }
    auto types = doc.child("Types");
    if (!types) {
        return {};
    }
    const auto absolute = [](std::string_view part) { return "/" + std::string(part); };

    std::set<std::string> drop;
    for (const auto& part : removed_parts) {
        drop.insert(absolute(part));
    }
    for (const auto& [part, ct] : added_overrides) {
        (void)ct;
        drop.insert(absolute(part));  // an existing Override is replaced by the new one
    }
    for (auto node = types.child("Override"); node;) {
        auto next = node.next_sibling("Override");
        if (drop.contains(node.attribute("PartName").as_string(""))) {
            types.remove_child(node);
        }
        node = next;
    }
    for (const auto& [part, ct] : added_overrides) {
        auto node = types.append_child("Override");
        node.append_attribute("PartName") = absolute(part).c_str();
        node.append_attribute("ContentType") = ct.c_str();
    }

    std::set<std::string> extensions;
    pugi::xml_node last_default;
    for (auto node : types.children("Default")) {
        extensions.insert(to_lower(node.attribute("Extension").as_string("")));
        last_default = node;
    }
    for (const auto& [ext, ct] : added_defaults) {
        if (!extensions.insert(to_lower(ext)).second) {
            continue;
        }
        auto node = last_default ? types.insert_child_after("Default", last_default)
                                 : types.prepend_child("Default");
        node.append_attribute("Extension") = ext.c_str();
        node.append_attribute("ContentType") = ct.c_str();
        last_default = node;
    }
    return save_xml(doc);
}

std::string relative_target(std::string_view from_part, std::string_view to_part) {
    const std::string from_dir = dir_of(from_part);
    std::size_t common = 0;  // length of the shared directory prefix, up to a '/'
    for (std::size_t i = 0; i < from_dir.size() && i < to_part.size() && from_dir[i] == to_part[i];
         ++i) {
        if (from_dir[i] == '/') {
            common = i + 1;
        }
    }
    std::string out;
    for (std::size_t i = common; i < from_dir.size(); ++i) {
        if (from_dir[i] == '/') {
            out += "../";
        }
    }
    return out + std::string(to_part.substr(common));
}

std::vector<std::byte> retarget_rels(std::span<const std::byte> xml,
                                     std::string_view source_part,
                                     std::string_view target_part,
                                     const std::map<std::string, std::string>& retarget) {
    pugi::xml_document doc;
    if (!load_xml(doc, xml)) {
        return {};
    }
    auto rels = doc.child("Relationships");
    if (!rels) {
        return {};
    }
    const std::string base = dir_of(source_part);
    for (auto node : rels.children("Relationship")) {
        if (std::string_view(node.attribute("TargetMode").as_string("")) == "External") {
            continue;
        }
        auto target = node.attribute("Target");
        const auto it = retarget.find(resolve_target(base, target.as_string("")));
        if (it != retarget.end()) {
            target.set_value(relative_target(target_part, it->second).c_str());
        }
    }
    return save_xml(doc);
}

std::vector<std::byte> rewrite_rels(std::span<const std::byte> xml,
                                    std::string_view source_part,
                                    const std::set<std::string>& removed_parts) {
    pugi::xml_document doc;
    if (!load_xml(doc, xml)) {
        return {};
    }
    std::string source(source_part);
    if (!source.empty() && source.front() == '/') {
        source.erase(source.begin());
    }
    const std::string base = dir_of(source);
    auto rels = doc.child("Relationships");
    if (!rels) {
        return {};
    }
    for (auto node = rels.child("Relationship"); node;) {
        auto next = node.next_sibling("Relationship");
        const bool external =
            std::string_view(node.attribute("TargetMode").as_string("")) == "External";
        if (!external &&
            removed_parts.contains(resolve_target(base, node.attribute("Target").as_string("")))) {
            rels.remove_child(node);
        }
        node = next;
    }
    return save_xml(doc);
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t h = kOffset;
    for (const std::byte b : bytes) {
        h ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b));
        h *= kPrime;
    }
    return h;
}

struct Package::Impl {
    void* reader = nullptr;
    std::string path;
    std::vector<std::string> parts;
    std::string error;  // why open() refused the file, when it was the limits

    // Lazily-populated caches (see lazy-parsing note in the header).
    bool ct_loaded = false;
    std::map<std::string, std::string> ct_overrides;  // "/ppt/.../x.xml" -> content type
    std::map<std::string, std::string> ct_defaults;   // lowercased extension -> content type
    std::map<std::string, std::vector<Relationship>> rels_cache;  // source part -> relationships
    std::map<std::string, std::uint64_t> hash_cache;              // part name -> fnv1a64

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
        hash_cache.clear();
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
    impl_->error.clear();

    impl_->reader = mz_zip_reader_create();
    if (impl_->reader == nullptr) {
        return false;
    }
    if (mz_zip_reader_open_file(impl_->reader, path.c_str()) != MZ_OK) {
        impl_->close();
        return false;
    }

    // Enumerate entries to build the part list, checking the declared sizes
    // against the decompression limits as we go.
    std::uint64_t total = 0;
    int err = mz_zip_reader_goto_first_entry(impl_->reader);
    while (err == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(impl_->reader, &info) == MZ_OK && info != nullptr &&
            info->filename != nullptr) {
            if (auto refused = check_limits(*info, total)) {
                impl_->error = std::move(*refused);
                impl_->close();
                impl_->parts.clear();
                return false;
            }
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

const std::string& Package::error() const noexcept {
    return impl_->error;
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
    const auto bytes = read_part(rels_part_for(key));
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

std::optional<std::uint64_t> Package::part_size(std::string_view name) const {
    if (!impl_->is_open()) {
        return std::nullopt;
    }
    const std::string name_str(name);
    if (mz_zip_reader_locate_entry(impl_->reader, name_str.c_str(), 0) != MZ_OK) {
        return std::nullopt;
    }
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(impl_->reader, &info) != MZ_OK || info == nullptr) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info->uncompressed_size);
}

std::vector<std::byte> Package::read_part_prefix(std::string_view name,
                                                 std::size_t max_bytes) const {
    if (!impl_->is_open()) {
        return {};
    }
    const std::string name_str(name);
    if (mz_zip_reader_locate_entry(impl_->reader, name_str.c_str(), 0) != MZ_OK) {
        return {};
    }
    if (mz_zip_reader_entry_open(impl_->reader) != MZ_OK) {
        return {};
    }
    std::vector<std::byte> bytes(max_bytes);
    std::size_t got = 0;
    while (got < max_bytes) {
        const int32_t n = mz_zip_reader_entry_read(
            impl_->reader, bytes.data() + got, static_cast<int32_t>(max_bytes - got));
        if (n <= 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    mz_zip_reader_entry_close(impl_->reader);
    bytes.resize(got);
    return bytes;
}

std::optional<std::uint64_t> Package::part_hash(std::string_view name) const {
    if (!impl_->is_open()) {
        return std::nullopt;
    }
    const std::string key(name);
    if (const auto it = impl_->hash_cache.find(key); it != impl_->hash_cache.end()) {
        return it->second;
    }
    if (!part_size(name)) {
        return std::nullopt;  // no such part; an empty part still hashes
    }
    const auto h = fnv1a64(read_part(name));
    impl_->hash_cache.emplace(key, h);
    return h;
}

namespace {

// Stream every entry of `reader` into `writer`, applying `changes`. Returns
// false on the first error. The reader's entry cursor is left wherever the
// walk stopped; callers don't rely on it.
bool copy_entries(void* reader, void* writer, const PartChanges& changes) {
    std::set<std::string> pending;  // replaced names not yet seen in the source
    for (const auto& entry : changes.replaced) {
        if (!changes.removed.contains(entry.first)) {  // removed wins over replaced
            pending.insert(entry.first);
        }
    }

    // New parts take the date of the archive's first entry, so the same
    // inputs always produce the same bytes (std::time would not).
    std::optional<std::time_t> first_date;
    int err = mz_zip_reader_goto_first_entry(reader);
    while (err == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || info == nullptr ||
            info->filename == nullptr) {
            return false;
        }
        if (!first_date) {
            first_date = info->modified_date;
        }
        const std::string name(info->filename);
        if (changes.removed.contains(name)) {
            // dropped
        } else if (const auto rep = changes.replaced.find(name); rep != changes.replaced.end()) {
            if (!write_entry(writer, *info, rep->second)) {
                return false;
            }
            pending.erase(name);
        } else if (mz_zip_writer_copy_from_reader(writer, reader) != MZ_OK) {
            return false;
        }
        err = mz_zip_reader_goto_next_entry(reader);
    }
    if (err != MZ_END_OF_LIST) {
        return false;
    }

    // Brand-new parts go at the end, in name order.
    for (const auto& name : pending) {
        mz_zip_file info{};
        info.filename = name.c_str();
        info.filename_size = static_cast<uint16_t>(name.size());
        info.version_madeby = 20;  // MS-DOS host, spec 2.0: what PowerPoint itself writes
        info.modified_date = first_date ? *first_date : dos_epoch();
        if (!write_entry(writer, info, changes.replaced.at(name))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool Package::write(const std::string& out_path, const PartChanges& changes) {
    if (!impl_->is_open()) {
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target(out_path);
    if (fs::is_symlink(target, ec)) {
        target = fs::weakly_canonical(target, ec);  // replace the file, not the link
        if (ec) {
            return false;
        }
    }
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    const fs::path temp =
        target.string() + ".pptctl-" + std::to_string(std::random_device{}()) + ".tmp";

    bool ok = false;
    {
        const ZipWriter writer;
        if (writer.handle == nullptr) {
            return false;
        }
        mz_zip_writer_set_compress_method(writer.handle, MZ_COMPRESS_METHOD_DEFLATE);
        mz_zip_writer_set_compress_level(writer.handle, MZ_COMPRESS_LEVEL_DEFAULT);
        if (mz_zip_writer_open_file(writer.handle, temp.string().c_str(), 0, 0) == MZ_OK) {
            // Office writes sizes and crc into the local header, not a trailing
            // data descriptor; some readers are strict about that.
            void* zip = nullptr;
            mz_zip_writer_get_zip_handle(writer.handle, &zip);
            mz_zip_set_data_descriptor(zip, 0);
            ok = copy_entries(impl_->reader, writer.handle, changes);
            if (mz_zip_writer_close(writer.handle) != MZ_OK) {
                ok = false;
            }
        }
    }
    if (!ok) {
        fs::remove(temp, ec);
        return false;
    }

    const bool in_place = same_file(target, fs::path(impl_->path));
    if (in_place) {
        // Windows can't rename over an open file; POSIX can, but reopening
        // keeps the Package's view consistent with the file everywhere.
        impl_->close();
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (in_place) {
            (void)open(impl_->path);  // best effort: put the reader back
        }
        return false;
    }
    if (in_place) {
        return open(impl_->path);
    }
    return true;
}

}  // namespace pptctl
