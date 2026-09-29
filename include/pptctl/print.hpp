// pptctl/print.hpp
//
// A minimal print helper wrapping std::format. Drops the fmt dependency in
// favour of the C++20 standard library — same {} placeholder syntax, no
// migration cost, no consteval drama when toolchains update.
//
// Requires C++20 (std::format). All target compilers we support have it:
// - Apple Clang ≥ 16 (Xcode 16+)
// - LLVM Clang ≥ 17 (with libc++ ≥ 17)
// - GCC ≥ 13 (with libstdc++ 13)
// - MSVC ≥ 19.30
//
// We avoid std::print (C++23) deliberately to stay portable on gcc-13.

#pragma once

#include <cstddef>
#include <format>
#include <iostream>
#include <ostream>
#include <string>
#include <utility>

namespace pptctl {

template <typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
    std::cout << std::format(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    std::cout << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

template <typename... Args>
void eprint(std::format_string<Args...> fmt, Args&&... args) {
    std::cerr << std::format(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void eprintln(std::format_string<Args...> fmt, Args&&... args) {
    std::cerr << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

template <typename... Args>
void print_to(std::ostream& os, std::format_string<Args...> fmt, Args&&... args) {
    os << std::format(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void println_to(std::ostream& os, std::format_string<Args...> fmt, Args&&... args) {
    os << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

// Human-readable byte count: "512 B", "1.5 KiB", "2.3 MiB", ...
inline std::string human_bytes(std::size_t n) {
    constexpr double kKi = 1024.0;
    auto v = static_cast<double>(n);
    const char* unit = "B";
    if (v >= kKi) {
        v /= kKi;
        unit = "KiB";
    }
    if (v >= kKi) {
        v /= kKi;
        unit = "MiB";
    }
    if (v >= kKi) {
        v /= kKi;
        unit = "GiB";
    }
    if (n < 1024) {
        return std::format("{} {}", n, unit);
    }
    return std::format("{:.1f} {}", v, unit);
}

}  // namespace pptctl
