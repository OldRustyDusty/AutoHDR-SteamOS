#!/usr/bin/env bash
# Install out/hdrket into this machine's Decky Loader and restart it.
# Decky's plugin folder belongs to root, so run this as root (sudo or pkexec).
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)/out/hdrket"
user_home=$(getent passwd "${SUDO_USER:-${PKEXEC_UID:+$(id -nu "$PKEXEC_UID")}}" | cut -d: -f6)
dest="$user_home/homebrew/plugins/hdrket"
[ -f "$src/plugin.json" ] || { echo "build the plugin first: scripts/package-decky.sh" >&2; exit 1; }
rm -rf "$dest"
cp -r "$src" "$dest"
systemctl restart plugin_loader
echo "Installed to $dest and restarted Decky."
