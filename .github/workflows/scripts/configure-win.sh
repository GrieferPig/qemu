#!/usr/bin/env bash

set -euo pipefail

TARGET=${TARGET:-xtensa-softmmu}
VERSION=${VERSION:-dev}

echo DBG
./configure --help

./configure \
    --bindir=bin \
    --datadir=share/qemu \
    --enable-gcrypt \
    --enable-sdl \
    --enable-pixman \
    --disable-slirp \
    --enable-stack-protector \
    --extra-cflags=-Wno-error \
    --prefix=${PWD}/install/qemu \
    --static \
    --target-list=${TARGET} \
    --with-pkgversion="${VERSION}" \
    --with-suffix="" \
    --without-default-features \
|| { cat meson-logs/meson-log.txt && false; }


# This fixes the issue that for some reason, meson is not able to determine correct
# paths for libiconv and libintl libraries from 'pkg-config --libs --static libgcrypt'.
MSYS_BASE=$(cygpath -m /)
MSYS_BASE_NINJA=${MSYS_BASE/:/\$:}
sed -i \
    -e "s|\"/mingw64/lib/libintl.dll.a\"|\"${MSYS_BASE}/mingw64/lib/libintl.dll.a\"|g" \
    -e "s| /mingw64/lib/libintl.dll.a| ${MSYS_BASE_NINJA}/mingw64/lib/libintl.dll.a|g" \
    -e "s|\"/mingw64/lib/libiconv.dll.a\"|\"${MSYS_BASE}/mingw64/lib/libiconv.dll.a\"|g" \
    -e "s| /mingw64/lib/libiconv.dll.a| ${MSYS_BASE_NINJA}/mingw64/lib/libiconv.dll.a|g" \
    build/build.ninja