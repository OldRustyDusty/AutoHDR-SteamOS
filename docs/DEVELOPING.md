# Developing AutoHDR for SteamOS (HDR-Ket)

Notes for contributors (and for picking the work up on another machine). The
README covers what the project does and how users install it.

## How the pieces fit

```
Game ── Vulkan (native, DXVK, vkd3d-proton)
  └─ libhdrket.so  Vulkan implicit layer, active only with ENABLE_HDRKET=1
       ├─ swaps the game's SDR swapchain for HDR10 (src/swapchain.cpp)
       ├─ one compute pass per frame: src/shaders/itm.comp (+ common.glsl)
       ├─ live settings + per-game/per-display profiles (src/params.cpp)
       └─ HTTP tuning page + API on :7331 (src/httpd.cpp, ui/index.html)
Decky plugin (decky/)
  ├─ main.py   installs the layer into ~/.local, reads EDID, edits profiles,
  │            talks to running games over the layer's HTTP API
  └─ src/      the quick access panel (React), launch-option helpers
```

The layer starts its HTTP server only after it has created an HDR swapchain
for the game. **If the phone page doesn't load and the panel's sliders do
nothing, the layer is not active in that game**: either it never loaded, or it
loaded and stepped aside ("passthrough"). The logs say which.

Internal names are `hdrket` everywhere (library, `ENABLE_HDRKET`,
`~/.config/hdrket`); only user-facing text says AutoHDR. Keep it that way so
existing launch options and profiles keep working.

## Files on a user's machine

| Path | What |
|---|---|
| `~/homebrew/plugins/hdrket/` | The Decky plugin (root-owned) |
| `~/.local/share/hdrket/libhdrket.so` | The layer, installed by the plugin |
| `~/.local/share/vulkan/implicit_layer.d/hdrket.json` | Layer manifest |
| `~/.local/state/hdrket/hdrket-<pid>.log` | Layer log, one per game process |
| `~/homebrew/logs/hdrket/` | Plugin (Python backend) log |
| `~/.config/hdrket/presets/steam-<appid>.conf` | Per-game settings |
| `~/.config/hdrket/displays/<display>.conf` | Per-display peak |

## Diagnosing "no effect" (for example the Steam Deck report)

Work through these in order; each one narrows it down.

1. **Is the layer installed?**
   `cat ~/.local/share/vulkan/implicit_layer.d/hdrket.json` should point at a
   `libhdrket.so` that exists. `objdump -T` on it should show only
   `hdrket_GetInstanceProcAddr` / `hdrket_GetDeviceProcAddr` and need glibc
   2.34 or older.
2. **Did the game get the launch option?** Steam > game > Properties >
   Launch Options should contain `ENABLE_HDRKET=1 %command%`.
3. **Did the layer load in the game?** Start the game, then look for a new
   `~/.local/state/hdrket/hdrket-<pid>.log`.
   - **No log at all:** the loader never loaded the layer. Check that the game
     is 64-bit (the layer is x86-64 only for now; 32-bit games, such as many
     D3D9 titles under DXVK, need a 32-bit build), and run a Vulkan program by
     hand with `ENABLE_HDRKET=1 VK_LOADER_DEBUG=layer` to see whether the
     loader finds the manifest. Games under Steam's runtime container
     (pressure-vessel) import layers from `~/.local/share/vulkan`; check that
     this still holds on the system in question.
   - **A log that ends in `passthrough: ...`:** the layer loaded but stepped
     aside. `no usable HDR surface format` means the display path offered no
     HDR formats: check that HDR is on in Steam's display settings, and look
     at the `surface HDR formats:` line. In Game Mode the formats come from
     gamescope's WSI layer (`VkLayer_FROG_gamescope_wsi`), which has to sit
     below ours; `VK_LOADER_DEBUG=layer` shows the order.
   - **A log with `HDR swapchain: ... HDR10_ST2084` and `tuning UI: http://...`:**
     the layer is working. If the panel still shows nothing, the problem is in
     the plugin: check `~/homebrew/logs/hdrket/` and that
     `curl localhost:7331/api/state` answers while the game runs.
4. **Phone page:** reachable only while step 3 shows the tuning UI line. The
   phone must be on the same network; check for a firewall on port 7331.

## Building

Needs meson, ninja, a C++20 compiler, glslang, the Vulkan headers,
libstdc++ (static), Python 3, Node.js 22 and pnpm 9.

On an immutable distro (Bazzite, SteamOS) use a distrobox:

```bash
# Fedora box for development builds and the plugin frontend
distrobox create --name hdrket-build --image registry.fedoraproject.org/fedora-toolbox:44
distrobox enter hdrket-build -- sudo dnf install -y meson ninja-build gcc-c++ glslang \
    vulkan-headers libstdc++-static nodejs-npm zip
distrobox enter hdrket-build -- sudo npm install -g pnpm@9

# Optional: Ubuntu 22.04 box for release builds that need only glibc 2.34
distrobox create --name hdrket-compat --image docker.io/library/ubuntu:22.04
distrobox enter hdrket-compat -- sudo apt-get install -y meson ninja-build g++ \
    glslang-tools libvulkan-dev python3 zip binutils
```

| Command | Does |
|---|---|
| `scripts/dev-install.sh` | Builds the layer and installs it into `~/.local` (a developer copy the plugin then leaves alone) |
| `scripts/package-decky.sh` | Builds layer + plugin into `out/autohdr-steamos-decky.zip` |
| `scripts/install-decky-local.sh` | Installs `out/hdrket` into this machine's Decky (run as root, e.g. `pkexec`) |
| `cd decky && pnpm test` | Launch-option helper tests |

Releases: push a `vX.Y.Z` tag. GitHub Actions builds on Ubuntu 22.04 and
attaches the plugin zip, `install.sh` and `AutoHDR-installer.desktop`. Bump
the version in `meson.build` and `decky/package.json` first.

## Testing without a game

`vkcube` exercises the whole path. Run it with the layer and Khronos validation
underneath (synchronization validation catches barrier and semaphore
mistakes):

```bash
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
ENABLE_HDRKET=1 HDRKET_PRESET=test HDRKET_DISPLAY=test-display \
vkcube --wsi wayland --c 600
```

`HDRKET_PRESET` / `HDRKET_DISPLAY` keep tests away from real profiles (delete
`~/.config/hdrket/presets/test.conf` and `displays/test-display.conf`
afterwards). While it runs, `curl localhost:7331/api/state` returns the live
state and `curl -X POST -d 'tonemap=clip' localhost:7331/api/settings` changes
settings.

On a KDE desktop, games under XWayland get no HDR formats; use a Wayland
client (`vkcube --wsi wayland`) or nest in `gamescope --hdr-enabled`, which
also reproduces the Game Mode path.

## Conventions

- One fused compute pass; no effect chains. Every setting applies next frame
  (no restarts).
- Never copy code from GPL projects (Special K, Lilium's ReShade shaders).
  Credit anything borrowed in `THIRD_PARTY_NOTICES.md` and the README.
- Commits end with a `Co-Authored-By:` line when an AI assistant wrote them.
