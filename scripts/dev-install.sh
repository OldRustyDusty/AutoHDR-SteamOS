#!/usr/bin/env bash
# Build and install HDR Ket into ~/.local as an implicit Vulkan layer.
# It only activates for processes started with ENABLE_HDRKET=1.
#
# On Bazzite/SteamOS the build runs in the `hdrket-build` distrobox
# (Fedora with meson, glslang and Vulkan headers); elsewhere it runs directly.
set -euo pipefail
cd "$(dirname "$0")/.."

build() {
    [ -d build ] || meson setup build --prefix="$HOME/.local" --libdir=lib64
    ninja -C build
    meson install -C build --quiet
}

if command -v meson >/dev/null && command -v glslangValidator >/dev/null; then
    build
else
    distrobox enter hdrket-build -- bash -c "cd \"$PWD\" && $(declare -f build); build"
fi

echo "Installed: $HOME/.local/share/vulkan/implicit_layer.d/hdrket.json"
echo "Enable per game in Steam launch options:  ENABLE_HDRKET=1 %command%"
