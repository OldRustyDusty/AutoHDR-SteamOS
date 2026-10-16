# HDR Ket

SDR→HDR inverse tone mapping for Linux games, as a Vulkan implicit layer.
Works below DXVK / vkd3d-proton / native Vulkan, so one code path covers
D3D9 through D3D12 and native titles. No injection, no hooks into the game,
no extra frame queuing.

Status: layer, HDR10/scRGB swapchain substitution, one fused compute pass,
live tuning page with a brightness readout, and per-game presets.

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

## Tuning

When a game starts with HDR Ket active, the log prints the tuning page address:

```
[hdrket info] tuning UI: http://192.168.0.130:7331
```

Open it on a phone or laptop. Every change applies on the next frame while the
game stays full screen. **Save for this game** writes
`~/.config/hdrket/presets/<game>.conf`, which loads automatically next time.

| Control | What it does |
|---|---|
| Peak brightness | Your display's real maximum. Find it with the test pattern: the bar where brightness stops increasing. Saved per display; tick *Peak for this game only* to give one game its own value on this display. |
| Middle grey | Overall brightness: where SDR 18% grey lands. SDR white sits at grey / 0.18. |
| Highlight expansion | How far bright highlights reach toward peak. 0 = plain SDR in an HDR container; above 100% they aim past peak and the tone mapper rolls them back. |
| Tone mapping | What happens above peak. **Off**: sent as is. **Clip**: cut at peak, hue kept. **Map**: BT.2390 roll-off from a fixed range. **Adaptive**: the same roll-off following each scene's brightest point, plus scene adaptation: the game's own brightest content (99.5th percentile) counts as white, so dim scenes keep their highlight pop, and dark scenes get a gentle shadow lift below middle grey. Reacts fast when a scene brightens, relaxes slowly when it darkens. |
| Adaptive strength | How far Adaptive follows the scene (0 = same as Map). |
| Content gamma | How the game was mastered: Linear, sRGB, 2.2 (most PC games), 2.4. |
| Saturation | Color intensity around each pixel's own brightness. |
| Gamut expansion | Pushes saturated colors toward BT.2020. Greys and low-chroma tones (skin) are protected; brightness is kept. |
| Black floor | Crush raised blacks. |
| Compare | HDR, Split (left half SDR), or SDR. |
| Measured on screen | Live peak, 99th percentile and average brightness, what the tone mapper is doing, and a histogram. |

### Presets and displays

- `~/.config/hdrket/displays/<display>.conf` holds each display's peak. The
  display is identified from its EDID (name plus a checksum), so two TVs behind
  the same AV receiver still get separate profiles. If the TV reports its peak
  in the EDID, that is the starting value. `HDRKET_DISPLAY=<name>` forces a
  profile.
- `~/.config/hdrket/presets/<game>.conf` holds the game's picture settings and
  any per-display peak overrides (`peak_nits@<display> = 830`). A plain
  `peak_nits` in a game preset applies on every display.

The page is served on all interfaces so a phone can reach it.
`HDRKET_BIND=127.0.0.1` keeps it local, `HDRKET_PORT` changes the port, and
`HDRKET_UI=0` turns it off.

## Logs

Messages go to stderr and to `~/.local/state/hdrket/hdrket-<pid>.log` (Game Mode
hides stderr). `HDRKET_LOG=debug` shows what was detected. If the log says
`passthrough: no usable HDR surface format`, the point above applies.

### Environment overrides

Any preset key can be set as `HDRKET_<KEY>`, applied on top of the saved preset:
`HDRKET_PEAK_NITS`, `HDRKET_MIDDLE_GREY_NITS`, `HDRKET_PAPER_WHITE_NITS`,
`HDRKET_EXPANSION`, `HDRKET_SATURATION`, `HDRKET_BLACK_FLOOR`, `HDRKET_EOTF`
(`linear`, `srgb`, `gamma22`, `gamma24`), `HDRKET_TONEMAP` (`off`, `clip`,
`map`, `adaptive`), `HDRKET_GAMUT_EXPANSION`, `HDRKET_ADAPT_RELEASE` (seconds),
`HDRKET_ADAPT_STRENGTH`,
`HDRKET_SPLIT`, `HDRKET_PATTERN`,
`HDRKET_BYPASS`. Also `HDRKET_PRESET=<name>` picks a preset file, and
`HDRKET_OUTPUT=hdr10|scrgb|off`.

HDR10 is the default output: PQ values are absolute nits on every compositor.
scRGB is relative on Wayland (KWin maps 1.0 to the desktop SDR brightness), so
it is opt-in for now.

## Licensing

MIT. Do not copy code from Special K (GPLv3) or Lilium's ReShade HDR shaders
(GPLv3). The highlight expansion follows the approach of PumboAutoHDR (MIT);
layer plumbing follows vkBasalt (zlib). Tone mapping implements the ITU-R BT.2390
EETF from the published recommendation.
