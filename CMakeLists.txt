cmake_minimum_required(VERSION 3.24)
project(pptctl
    VERSION 0.4.0
    DESCRIPTION "A fast CLI for .pptx files"
    LANGUAGES C CXX
)

# --- language -----------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release if not specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# --- warnings (only for our own code, not deps) -------------------------------
add_library(pptctl_warnings INTERFACE)
if(MSVC)
    target_compile_options(pptctl_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(pptctl_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion)
endif()

# --- fetch deps ---------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Some of our deps still ship CMakeLists with cmake_minimum_required(VERSION
# 2.x) which CMake 4 rejects outright. This pretends those asked for ≥3.5,
# matching `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` on the command line.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Minimum policy version for FetchContent dependencies" FORCE)

# CLI11 — argument parsing. Header-only.
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.5.0
    GIT_SHALLOW TRUE
)

# pugixml — XML parser.
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
)

# nlohmann/json — JSON output. The release tarball (~110 KB) rather than the
# repository, whose shallow clone is ~195 MB of test data.
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# minizip-ng — ZIP I/O. Disable compression formats we don't need.
set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
set(MZ_LZMA OFF CACHE BOOL "" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
# No encryption: pptctl never reads or writes encrypted packages, and with crypto
# on minizip appends a SHA-256 extra field to every entry it deflates — Office
# never emits one, and the write path must produce entries that look like Office's.
set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
set(MZ_WZAES OFF CACHE BOOL "" FORCE)
set(MZ_FETCH_LIBS ON CACHE BOOL "" FORCE)
# On Windows there's no system zlib, so minizip-ng fetches zlib-ng, whose `zlib`
# target is an ALIAS. minizip-ng's install() rules then choke on installing an
# alias target. We only consume these deps from the build tree, never install
# them, so skip their install rules entirely. (The MINIZIP::minizip alias we
# link against is created independently of the install block.)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
# When it has to fetch zlib-ng, build it with the classic zlib API (zlib.h,
# inflate/deflate) rather than its native zng_ one, so the same zlib also
# serves libpng (see the image codecs section below).
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    minizip-ng
    GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
    GIT_TAG 4.0.10
    GIT_SHALLOW TRUE
)

# doctest — testing. Header-only-ish.
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(cli11 pugixml nlohmann_json minizip-ng doctest)

# --- image codecs (#102) -------------------------------------------------------
# libpng — PNG decode/encode. It finds zlib with find_package(ZLIB); make that
# resolve to the zlib minizip-ng already uses. On macOS and Linux minizip-ng
# uses the system zlib (or Apple's libcompression), which libpng finds the
# same way. Where there is none (Windows), minizip-ng has fetched zlib-ng in
# zlib-compat mode instead: alias its static target as ZLIB::ZLIB and answer
# libpng's find_package(ZLIB) from FetchContent's redirects directory, so
# libpng links the same zlib rather than failing to find one.
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    if(TARGET zlibstatic)
        set(_pptctl_zlib zlibstatic)
    elseif(TARGET zlib)
        set(_pptctl_zlib zlib)
    else()
        message(FATAL_ERROR "libpng needs zlib, but neither a system zlib nor minizip-ng's "
                            "fetched zlib-ng was found")
    endif()
    get_target_property(_pptctl_zlib_aliased ${_pptctl_zlib} ALIASED_TARGET)
    if(_pptctl_zlib_aliased)
        set(_pptctl_zlib ${_pptctl_zlib_aliased})
    endif()
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS ${_pptctl_zlib})
    endif()
    # zlib-ng generates zlib.h/zconf.h in its binary dir. libpng also passes
    # ZLIB_INCLUDE_DIRS to the preprocessor run that generates pnglibconf.h,
    # which must see the same zlib.h as the build.
    get_target_property(_pptctl_zlib_bin ${_pptctl_zlib} BINARY_DIR)
    get_target_property(_pptctl_zlib_src ${_pptctl_zlib} SOURCE_DIR)
    file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/zlib-config.cmake"
        "# Written by pptctl: zlib is minizip-ng's ${_pptctl_zlib} target (ZLIB::ZLIB).\n"
        "set(ZLIB_FOUND TRUE)\n"
        "set(ZLIB_INCLUDE_DIRS \"${_pptctl_zlib_bin};${_pptctl_zlib_src}\")\n"
        "set(ZLIB_INCLUDE_DIR \"${_pptctl_zlib_bin}\")\n"
        "set(ZLIB_LIBRARIES ZLIB::ZLIB)\n")
    message(STATUS "libpng: using minizip-ng's zlib target ${_pptctl_zlib}")
endif()

set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
# libpng's stock configuration header (its default feature set) rather than one
# generated with awk at build time: it then exists right after configure, which
# the clang-tidy CI job (configure only) needs to parse png.h, and the build
# needs no awk.
set(PNG_LIBCONF_HEADER "${FETCHCONTENT_BASE_DIR}/libpng-src/scripts/pnglibconf.h.prebuilt"
    CACHE FILEPATH "" FORCE)
# libpng only looks for awk when it generates the header, but a cached AWK from
# an older configure makes it expect generated sources it no longer writes.
unset(AWK CACHE)
FetchContent_Declare(
    libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng.git
    GIT_TAG v1.6.59
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libpng)
# libpng's build-tree include directories are not marked SYSTEM; mark them so
# png.h is held to the same no-warnings boundary as the other deps.
get_target_property(_pptctl_png_inc png_static INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(png_static PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pptctl_png_inc}")

# libjpeg-turbo — JPEG decode/encode; built as an external sub-build, see the file.
include(cmake/libjpeg-turbo.cmake)

# Suppress warnings from third-party code so they don't drown out ours.
if(NOT MSVC)
    if(TARGET minizip-ng)
        target_compile_options(minizip-ng PRIVATE -w)
    endif()
    if(TARGET pugixml-static)
        target_compile_options(pugixml-static PRIVATE -w)
    endif()
    if(TARGET png_static)
        target_compile_options(png_static PRIVATE -w)
    endif()
endif()

# --- internal library: what will eventually become libpptctl -----------------
add_library(pptctl_lib STATIC
    src/lib/package.cpp
    src/lib/part_graph.cpp
    src/lib/presentation.cpp
    src/lib/slide.cpp
    src/lib/image.cpp
    src/lib/image_codec.cpp
    src/lib/diff.cpp
    src/lib/diff_text.cpp
    src/lib/diff_shape.cpp
    src/lib/diff_picture.cpp
    src/lib/diff_table.cpp
    src/lib/diff_chart.cpp
    src/lib/optimize.cpp
    src/lib/optimize_layouts.cpp
    src/lib/optimize_fonts.cpp
    src/lib/optimize_crop.cpp
    src/lib/optimize_images.cpp
    src/lib/resample.cpp
    src/lib/optimize_orphans.cpp
)
target_include_directories(pptctl_lib PUBLIC include)
target_link_libraries(pptctl_lib
    PUBLIC
        pugixml::pugixml
        nlohmann_json::nlohmann_json
    PRIVATE
        MINIZIP::minizip
        png_static
        libjpeg_turbo::jpeg
        pptctl_warnings
)

# --- CLI binary --------------------------------------------------------------
add_executable(pptctl
    src/main.cpp
    src/cmd/info.cpp
    src/cmd/extract.cpp
    src/cmd/lint.cpp
    src/cmd/diff.cpp
    src/cmd/optimize.cpp
)
target_link_libraries(pptctl
    PRIVATE
        pptctl_lib
        CLI11::CLI11
        pptctl_warnings
)

# --- tests -------------------------------------------------------------------
enable_testing()
add_executable(pptctl_tests
    tests/test_main.cpp
    tests/test_package.cpp
    tests/test_package_write.cpp
    tests/test_part_graph.cpp
    tests/test_slide.cpp
    tests/test_extract.cpp
    tests/test_lint.cpp
    tests/test_image.cpp
    tests/test_diff.cpp
    tests/test_diff_text.cpp
    tests/test_diff_shape.cpp
    tests/test_diff_picture.cpp
    tests/test_diff_table.cpp
    tests/test_diff_chart.cpp
    tests/test_optimize.cpp
    tests/test_optimize_layouts.cpp
    tests/test_optimize_fonts.cpp
    tests/test_optimize_crop.cpp
    tests/test_optimize_images.cpp
    tests/test_optimize_orphans.cpp
    tests/test_optimize_golden.cpp
    src/cmd/extract.cpp
    src/cmd/lint.cpp
    src/cmd/diff.cpp
    src/cmd/optimize.cpp
)
target_link_libraries(pptctl_tests
    PRIVATE
        pptctl_lib
        doctest::doctest
        pptctl_warnings
)
target_compile_definitions(pptctl_tests PRIVATE
    PPTCTL_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    PPTCTL_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden"
)
add_test(NAME unit COMMAND pptctl_tests)

# --- install -----------------------------------------------------------------
install(TARGETS pptctl RUNTIME DESTINATION bin)
