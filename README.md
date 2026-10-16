# HDR Ket

SDR→HDR inverse tone mapping for Linux games, as a Vulkan implicit layer.
Works below DXVK / vkd3d-proton / native Vulkan, so one code path covers
D3D9 through D3D12 and native titles. No injection, no hooks into the game,
no extra frame queuing.

Status: **Sprint 1** — layer skeleton, swapchain format substitution, single
fused compute pass with the five core knobs (set by environment variables for
now; the config file and browser tuning UI come next).

## How it works

When a game creates an SDR swapchain, HDR Ket creates the real swapchain as
HDR (scRGB fp16 by default, or HDR10 PQ) and hands the game "shadow" images in
the format it asked for. At present time one compute dispatch reads the
game's frame, expands it to HDR, and writes the real swapchain image.

## Build and install (Bazzite / Fedora)

```bash
# one-time: a build box with the toolchain
distrobox create --name hdrket-build --image registry.fedoraproject.org/fedora-toolbox:44
distrobox enter hdrket-build -- sudo dnf install -y meson ninja-build gcc-c++ glslang \
    vulkan-headers libstdc++-static

./scripts/dev-install.sh   # builds, installs into ~/.local
```

## Use

Steam launch options:

```
ENABLE_HDRKET=1 %command%
```

The display path must offer HDR formats to the game:

- **Game Mode / gamescope** (Steam Deck, Bazzite Game Mode): turn HDR on in
  Steam's display settings. Gamescope's WSI layer exposes HDR to the game.
- **KDE Plasma desktop**: enable HDR in Display Configuration. XWayland windows
  get no HDR formats, so the game must run natively on Wayland:
  Proton games: `PROTON_ENABLE_WAYLAND=1`; Godot games: `--display-driver wayland`;
  or nest the game in `gamescope --hdr-enabled`.

`HDRKET_LOG=debug` shows what was detected. If the log says
`passthrough: no usable HDR surface format`, the point above applies.

### Knobs (temporary environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `HDRKET_PEAK_NITS` | 1000 | Display peak brightness |
| `HDRKET_PAPER_WHITE` | 200 | Brightness of SDR white / UI |
| `HDRKET_EXPANSION` | 0.5 | 0 = SDR in an HDR container, 1 = SDR white reaches peak |
| `HDRKET_SATURATION` | 1.0 | 1 = unchanged |
| `HDRKET_BLACK_FLOOR` | 0 | Crush raised blacks (SDR-relative, e.g. 0.02) |
| `HDRKET_SPLIT` | 0 | Show the left fraction of the screen as SDR reference (e.g. 0.5) |
| `HDRKET_EOTF` | gamma 2.2 | `srgb` for the piecewise sRGB curve |
| `HDRKET_OUTPUT` | auto (scRGB) | `scrgb`, `hdr10`, or `off` |

## Licensing

MIT. Do not copy code from Special K (GPLv3) or Lilium's ReShade HDR shaders
(GPLv3). The highlight expansion follows the approach of PumboAutoHDR (MIT);
layer plumbing follows vkBasalt (zlib).
