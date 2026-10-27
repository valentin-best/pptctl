# libjpeg-turbo — JPEG decode/encode for the optimize image pass (#102).
#
# libjpeg-turbo's build system refuses add_subdirectory() (a FATAL_ERROR at the
# top of its CMakeLists.txt), so FetchContent only downloads the sources here
# (SOURCE_SUBDIR points at a directory with no CMakeLists.txt, which makes
# FetchContent_MakeAvailable skip add_subdirectory). The library is then
# configured, built and installed into the build tree *at configure time*, with
# the same compiler, build type and flags as pptctl, and exposed as the
# imported target `libjpeg_turbo::jpeg`.
#
# Building at configure time rather than with ExternalProject_Add means the
# headers and the archive exist before any of our translation units compile,
# so a configure alone yields a usable compile_commands.json (the CI clang-tidy
# job only configures) and Ninja needs no BUILD_BYPRODUCTS bookkeeping. A
# stamp of the inputs skips the sub-build on re-configure when nothing changed.
#
# SIMD: on by default, with libjpeg-turbo's own clean fallback. Arm64 (Neon)
# needs nothing extra; x86-64 needs NASM or Yasm on PATH and falls back to the
# portable C code without it. The chosen variant is printed at configure time.

set(PPTCTL_LIBJPEG_TURBO_TAG 3.2.0)

FetchContent_Declare(
    libjpeg_turbo
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG ${PPTCTL_LIBJPEG_TURBO_TAG}
    GIT_SHALLOW TRUE
    # No CMakeLists.txt there: download only, never add_subdirectory.
    SOURCE_SUBDIR pptctl-no-subproject
)
FetchContent_MakeAvailable(libjpeg_turbo)

set(_ljt_build "${CMAKE_BINARY_DIR}/_deps/libjpeg_turbo-ext-build")
set(_ljt_prefix "${CMAKE_BINARY_DIR}/_deps/libjpeg_turbo-install")
if(MSVC)
    set(_ljt_lib "${_ljt_prefix}/lib/jpeg-static.lib")
    set(_ljt_c_flags "${CMAKE_C_FLAGS} /w")
else()
    set(_ljt_lib "${_ljt_prefix}/lib/libjpeg.a")
    set(_ljt_c_flags "${CMAKE_C_FLAGS} -w")
endif()

set(_ljt_args
    -G "${CMAKE_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_C_FLAGS=${_ljt_c_flags}"
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DCMAKE_INSTALL_PREFIX=${_ljt_prefix}"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_INSTALL_INCLUDEDIR=include"
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=OFF
    -DWITH_JAVA=OFF
    -DWITH_TOOLS=OFF
    -DWITH_TESTS=OFF
    -DWITH_SIMD=ON
    -DREQUIRE_SIMD=OFF
    # Link against the same (DLL) C run-time as the rest of pptctl; the default
    # static CRT would clash with it at link time.
    -DWITH_CRT_DLL=ON
)
if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND _ljt_args "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _ljt_args "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

set(_ljt_stamp_file "${_ljt_build}/pptctl-stamp.txt")
set(_ljt_stamp "${PPTCTL_LIBJPEG_TURBO_TAG};${libjpeg_turbo_SOURCE_DIR};${_ljt_args}")
set(_ljt_old_stamp "")
if(EXISTS "${_ljt_stamp_file}")
    file(READ "${_ljt_stamp_file}" _ljt_old_stamp)
endif()

if(NOT EXISTS "${_ljt_lib}" OR NOT _ljt_old_stamp STREQUAL _ljt_stamp)
    message(STATUS "libjpeg-turbo ${PPTCTL_LIBJPEG_TURBO_TAG}: building into ${_ljt_prefix}")
    file(REMOVE_RECURSE "${_ljt_build}" "${_ljt_prefix}")
    file(MAKE_DIRECTORY "${_ljt_build}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${_ljt_args} -S "${libjpeg_turbo_SOURCE_DIR}" -B "${_ljt_build}"
        RESULT_VARIABLE _ljt_result
        OUTPUT_VARIABLE _ljt_out
        ERROR_VARIABLE _ljt_out
    )
    if(NOT _ljt_result EQUAL 0)
        message(FATAL_ERROR "libjpeg-turbo configure failed:\n${_ljt_out}")
    endif()
    string(REGEX MATCH "SIMD extensions: [^\n]*" _ljt_simd "${_ljt_out}")
    file(WRITE "${_ljt_build}/pptctl-simd.txt" "${_ljt_simd}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_ljt_build}" --target install --parallel
        RESULT_VARIABLE _ljt_result
        OUTPUT_VARIABLE _ljt_out
        ERROR_VARIABLE _ljt_out
    )
    if(NOT _ljt_result EQUAL 0 OR NOT EXISTS "${_ljt_lib}")
        message(FATAL_ERROR "libjpeg-turbo build failed:\n${_ljt_out}")
    endif()
    file(WRITE "${_ljt_stamp_file}" "${_ljt_stamp}")
endif()

if(EXISTS "${_ljt_build}/pptctl-simd.txt")
    file(READ "${_ljt_build}/pptctl-simd.txt" _ljt_simd)
    message(STATUS "libjpeg-turbo ${PPTCTL_LIBJPEG_TURBO_TAG}: ${_ljt_simd}")
endif()

# Imported targets' include directories are treated as system headers, so
# jpeglib.h never trips pptctl_warnings.
add_library(libjpeg_turbo::jpeg STATIC IMPORTED GLOBAL)
set_target_properties(libjpeg_turbo::jpeg PROPERTIES
    IMPORTED_LOCATION "${_ljt_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_ljt_prefix}/include"
)
