#!/usr/bin/env bash
# Build the Decky plugin: the Vulkan layer (stripped) plus the plugin frontend
# and backend, assembled as out/hdrket/ and zipped as out/hdrket-decky-<v>.zip.
#
# On Bazzite/SteamOS this runs in the `hdrket-build` distrobox, which needs
# meson, glslang, the Vulkan headers, libstdc++-static, nodejs and pnpm.
set -euo pipefail
cd "$(dirname "$0")/.."

build() {
    [ -d build-release ] || meson setup build-release --buildtype=release
    ninja -C build-release
    (cd decky && pnpm install --frozen-lockfile=false --silent && pnpm test && pnpm run build)

    version=$(sed -n 's/.*"version": "\(.*\)".*/\1/p' decky/package.json)
    out=out/hdrket
    rm -rf out && mkdir -p "$out/bin" "$out/dist"
    cp decky/plugin.json decky/package.json decky/main.py "$out/"
    cp decky/dist/index.js "$out/dist/"
    cp LICENSE "$out/"
    cp README.md "$out/"
    strip --strip-unneeded -o "$out/bin/libhdrket.so" build-release/libhdrket.so
    cp layer/hdrket.json.in "$out/bin/"
    (cd out && zip -qr "hdrket-decky-$version.zip" hdrket)
    echo "Built out/hdrket-decky-$version.zip ($(du -h "out/hdrket-decky-$version.zip" | cut -f1))"
}

if command -v meson >/dev/null && command -v pnpm >/dev/null; then
    build
else
    distrobox enter hdrket-build -- bash -c "cd \"$PWD\" && $(declare -f build); build"
fi
