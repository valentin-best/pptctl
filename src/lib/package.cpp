// pptctl/package.cpp — real implementation backed by minizip-ng.

#include <pptctl/package.hpp>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pptctl {

struct Package::Impl {
    void* reader = nullptr;
    std::string path;
    std::vector<std::string> parts;

    ~Impl() { close(); }

    void close() {
        if (reader != nullptr) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            reader = nullptr;
        }
    }

    [[nodiscard]] bool is_open() const noexcept { return reader != nullptr; }
};

Package::Package() : impl_(new Impl()) {}
Package::~Package() { delete impl_; }

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

bool Package::is_open() const noexcept { return impl_->is_open(); }

std::size_t Package::part_count() const noexcept { return impl_->parts.size(); }

std::vector<std::string> Package::part_names() const { return impl_->parts; }

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
        if (mz_zip_reader_entry_save_buffer(impl_->reader, bytes.data(),
                                            static_cast<int32_t>(bytes.size())) != MZ_OK) {
            return {};
        }
    }
    return bytes;
}

}  // namespace pptctl
