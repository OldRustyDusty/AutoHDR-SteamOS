#!/usr/bin/env bash
# Build the Decky plugin: the Vulkan layer (stripped) plus the plugin frontend
# and backend, assembled as out/hdrket/ and zipped as
# out/autohdr-steamos-decky.zip.
#
# With meson and pnpm on PATH (CI) everything builds here. On Bazzite/SteamOS
# the layer builds in the `hdrket-compat` distrobox (Ubuntu 22.04, so the
# library runs on systems with glibc 2.34 or newer) when it exists, else in
# `hdrket-build`; the frontend builds in `hdrket-build` (Node.js + pnpm).
set -euo pipefail
cd "$(dirname "$0")/.."

build_layer() {
    [ -d build-release ] || meson setup build-release --buildtype=release
    ninja -C build-release
}

build_frontend() {
    (cd decky && pnpm install --silent && pnpm test && pnpm run build)
}

assemble() {
    out=out/hdrket
    rm -rf out && mkdir -p "$out/bin" "$out/dist"
    cp decky/plugin.json decky/package.json decky/main.py "$out/"
    cp decky/dist/index.js "$out/dist/"
    cp LICENSE README.md THIRD_PARTY_NOTICES.md "$out/"
    cp -r LICENSES "$out/"
    strip --strip-unneeded -o "$out/bin/libhdrket.so" build-release/libhdrket.so
    cp layer/hdrket.json.in "$out/bin/"
    (cd out && zip -qr autohdr-steamos-decky.zip hdrket)
    echo "Built out/autohdr-steamos-decky.zip ($(du -h out/autohdr-steamos-decky.zip | cut -f1))"
}

in_box() { distrobox enter "$1" -- bash -c "cd \"$PWD\" && $(declare -f "$2"); $2"; }

if command -v meson >/dev/null && command -v pnpm >/dev/null; then
    build_layer
    build_frontend
    assemble
else
    if distrobox list 2>/dev/null | grep -q hdrket-compat; then
        rm -rf build-release # never mix builds from the two boxes
        in_box hdrket-compat build_layer
    else
        in_box hdrket-build build_layer
    fi
    in_box hdrket-build build_frontend
    in_box hdrket-build assemble
fi
