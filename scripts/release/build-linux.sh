#!/bin/sh
# Build the Linux release binary: fully static against musl, so it runs on any
# distribution regardless of its glibc. Runs inside an Alpine container; the
# release workflow calls it the same way:
#
#   docker run --rm -v "$PWD":/src -w /src alpine:3.22 scripts/release/build-linux.sh
#
# Writes build/dist/pptctl, stripped, after the tests pass on the unstripped
# build.

set -eu

# samurai is Alpine's `ninja` (the ninja-build package keeps its binary off PATH).
apk add --no-cache -q build-base cmake samurai nasm zlib-dev zlib-static linux-headers git

cmake --preset linux-release -B build/dist \
    -DCMAKE_EXE_LINKER_FLAGS=-static \
    -DZLIB_USE_STATIC_LIBS=ON
cmake --build build/dist --parallel
ctest --test-dir build/dist --output-on-failure

# A static executable has no program interpreter (ld.so) to ask for.
if readelf -l build/dist/pptctl | grep -q INTERP; then
    echo "build/dist/pptctl is dynamically linked" >&2
    readelf -d build/dist/pptctl >&2
    exit 1
fi
strip build/dist/pptctl
build/dist/pptctl --version
