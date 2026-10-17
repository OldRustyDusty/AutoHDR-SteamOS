#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# AutoHDR for SteamOS (HDR-Ket) installer: puts the latest release into
# Decky Loader. Nothing is compiled; it downloads the ready-made plugin.
#
#   bash install.sh               install or update
#   bash install.sh --uninstall   remove the plugin and its Vulkan layer
#
# Runs in a terminal (prompts there) or from AutoHDR-installer.desktop
# (password and messages in dialogs). Needs Decky Loader already installed.
set -euo pipefail

REPO="OldRustyDusty/AutoHDR-SteamOS"
ZIP_URL="${HDRKET_ZIP_URL:-https://github.com/$REPO/releases/latest/download/autohdr-steamos-decky.zip}"
PLUGINS="${HDRKET_PLUGINS_DIR:-$HOME/homebrew/plugins}"
TITLE="AutoHDR for SteamOS"

# Dialogs when there's no terminal to talk to (double-clicked .desktop file).
gui() { [ -z "${HDRKET_NO_GUI:-}" ] && [ ! -t 1 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; }
say() {
    echo "$1"
    if gui; then zenity --info --title="$TITLE" --width=420 --text="$1" 2>/dev/null || true; fi
}
fail() {
    echo "Error: $1" >&2
    if gui; then zenity --error --title="$TITLE" --width=420 --text="$1" 2>/dev/null || true; fi
    exit 1
}

# Decky's plugin folder belongs to root. Ask once, in the terminal or in a
# password dialog, and only when the folder isn't writable already.
as_root() {
    if [ -w "$PLUGINS" ] || [ "$(id -u)" = 0 ]; then
        "$@"
    elif [ -t 1 ] || ! command -v pkexec >/dev/null; then
        sudo "$@"
    else
        pkexec "$@"
    fi
}

restart_decky() {
    [ -n "${HDRKET_NO_RESTART:-}" ] && return 0
    as_root systemctl restart plugin_loader
}

[ -d "$PLUGINS" ] || fail "Decky Loader isn't installed. Install it first from https://decky.xyz, then run this again."

if [ "${1:-}" = "--uninstall" ]; then
    as_root rm -rf "$PLUGINS/hdrket"
    restart_decky || true
    # The plugin put the Vulkan layer in your home folder; take it out too.
    manifest="$HOME/.local/share/vulkan/implicit_layer.d/hdrket.json"
    if grep -q "$HOME/.local/share/hdrket/libhdrket.so" "$manifest" 2>/dev/null; then
        rm -f "$manifest"
    fi
    rm -rf "$HOME/.local/share/hdrket"
    say "AutoHDR is uninstalled. Your per-game settings are still in ~/.config/hdrket if you reinstall later."
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Downloading the latest AutoHDR release..."
curl -fsSL --retry 2 -o "$tmp/autohdr.zip" "$ZIP_URL" || fail "Couldn't download $ZIP_URL. Check your internet connection."
python3 -m zipfile -e "$tmp/autohdr.zip" "$tmp" || fail "The download is damaged. Try again."
[ -f "$tmp/hdrket/plugin.json" ] || fail "The download doesn't contain the plugin. Try again later."
version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$tmp/hdrket/package.json")
chmod -R a+rX "$tmp/hdrket"

echo "Installing AutoHDR $version into Decky (this needs your password)..."
as_root bash -c 'rm -rf "$1/hdrket" && cp -r "$2" "$1/hdrket"' _ "$PLUGINS" "$tmp/hdrket" \
    || fail "Installing needs your password. On a Steam Deck that has never had one, open Konsole, run passwd to set one, then run this again."
restart_decky || fail "AutoHDR is installed, but Decky didn't restart. Reboot to finish."

say "AutoHDR $version is installed.

Switch to Game Mode, open the Decky menu and choose AutoHDR. Make sure HDR is on in Steam's display settings."
