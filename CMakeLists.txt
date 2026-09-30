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

# nlohmann/json — JSON output.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

# minizip-ng — ZIP I/O. Disable compression formats we don't need.
set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
set(MZ_LZMA OFF CACHE BOOL "" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
set(MZ_FETCH_LIBS ON CACHE BOOL "" FORCE)
# On Windows there's no system zlib, so minizip-ng fetches zlib-ng, whose `zlib`
# target is an ALIAS. minizip-ng's install() rules then choke on installing an
# alias target. We only consume these deps from the build tree, never install
# them, so skip their install rules entirely. (The MINIZIP::minizip alias we
# link against is created independently of the install block.)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
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

# Suppress warnings from third-party code so they don't drown out ours.
if(NOT MSVC)
    if(TARGET minizip-ng)
        target_compile_options(minizip-ng PRIVATE -w)
    endif()
    if(TARGET pugixml-static)
        target_compile_options(pugixml-static PRIVATE -w)
    endif()
endif()

# --- internal library: what will eventually become libpptctl -----------------
add_library(pptctl_lib STATIC
    src/lib/package.cpp
    src/lib/presentation.cpp
    src/lib/slide.cpp
    src/lib/image.cpp
    src/lib/diff.cpp
    src/lib/diff_text.cpp
    src/lib/diff_shape.cpp
    src/lib/diff_picture.cpp
    src/lib/diff_table.cpp
    src/lib/diff_chart.cpp
)
target_include_directories(pptctl_lib PUBLIC include)
target_link_libraries(pptctl_lib
    PUBLIC
        pugixml::pugixml
        nlohmann_json::nlohmann_json
    PRIVATE
        MINIZIP::minizip
        pptctl_warnings
)

# --- CLI binary --------------------------------------------------------------
add_executable(pptctl
    src/main.cpp
    src/cmd/info.cpp
    src/cmd/extract.cpp
    src/cmd/lint.cpp
    src/cmd/diff.cpp
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
    src/cmd/extract.cpp
    src/cmd/lint.cpp
    src/cmd/diff.cpp
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
