# vkhdr — Project Handoff

**A native Vulkan implicit layer for SDR→HDR injection on Linux.**

> Working name. Alternatives considered: `vkAutoHDR`, `hdrjack`. **Pick a final name before the first commit** so symbols, the layer manifest, and env var prefixes don't need renaming later.

This document is the seed context for Claude Code. It captures the problem, the architecture decision and the reasoning behind it, hard product requirements, prior art, and a sprint plan. Read it fully before writing code.

---

## 1. Problem statement

There is no usable SDR→HDR injection solution on Linux. The existing options all fail, and they fail for reasons that are structural rather than fixable by configuration.

### Special K under Proton — tested, unusable

Tested extensively on Steam Deck OLED. Results:

- **~3 of 4 games do not launch at all**, even after extensive troubleshooting.
- Of those that do launch, most suffer **input lag and roughly halved framerate**.
- Worst cases exhibit **~1 second of input lag** — comparable to frame generation / LSFG — and are unplayable.

Root causes (diagnosed, not speculation):

| Symptom | Cause |
|---|---|
| Won't launch | SK's init path touches `D3DKMT*` kernel thunks, `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO`, NVAPI, and CBT global hookchain setup, all wrapped in SEH. Wine stubs or fails most of it. `UsingWINE=true` gates some but not all. |
| FPS halving | SK interposes the swapchain and adds a full-res fp16 copy per frame on top of DXVK's own copies. `UseFlipDiscard=true` and the whole flip-model override are inert under Proton — Wine's DXGI is not Windows' DXGI, DXVK owns presentation, and direct scanout is lost. Paying for an optimization that cannot apply. |
| ~1s input lag | SK's frame limiter uses Windows high-resolution waitable timers plus `QueryPerformanceCounter` spin-waits. Wine's timer resolution is coarser, so the spin-wait degenerates into a busy loop — catastrophic on the Deck's 4C/8T APU. SK's present queuing then stacks on top of DXVK's frame-latency handling: buffering-of-buffering, input sampled at the wrong end of a deep present queue. |

**Conclusion: not tunable. Abandoned.**

### ReShade + Lilium / PumboAutoHDR — rejected without further testing on Linux

Tested on **Windows**, where it has the best possible chance, and it was still a nightmare:

- Matching the correct scRGB / output value is extremely difficult.
- Many games simply do not work at all.
- Games that do work take **~2–3 hours to tune**, with multiple game restarts.
- Compare: **Special K takes ~2–3 minutes** for equivalent results.

Adding Proton/Wine on top of that is not worth attempting. This path is closed.

### The gap that matters

**2–3 minutes vs 2–3 hours is the entire product thesis.** Section 4 decomposes that gap into hard requirements. Everything else in this document is downstream of it.

---

## 2. Architecture

Two pieces. Only one of them needs to be built.

### Piece A — render target precision (already exists, native Linux, use as-is)

**Lilium's (EndlesslyFlowering) DXVK HDR-mod fork** does render target format upgrading *inside the translation layer*. This is the piece Special K does badly on Linux and DXVK does for free — zero injection, zero hooks, zero Win32, no Wine involvement. It is the translation layer, not something fighting it.

Repo: `https://github.com/EndlesslyFlowering/dxvk` (branch `HDR-mod`)
Docs: `HDR-mod.md` on that branch

Working `dxvk.conf` shape (D3D9 example):

```ini
d3d9.enableRenderTargetUpgrades      = true
d3d9.upgrade_RGBA8_UNORM_renderTargetTo  = rgba16_sfloat
d3d9.upgrade_RGBX8_UNORM_renderTargetTo  = rgba16_sfloat
d3d9.upgrade_BGRA8_UNORM_renderTargetTo  = rgba16_sfloat
d3d9.upgrade_BGRX8_UNORM_renderTargetTo  = rgba16_sfloat
d3d9.upgrade_RGB10A2_UNORM_renderTargetTo = rgba16_sfloat
d3d9.upgrade_BGR10A2_UNORM_renderTargetTo = rgba16_sfloat
d3d9.upgrade_RGBA16_UNORM_renderTargetTo  = rgba32_sfloat
d3d9.enableBackBufferUpgrade         = true
d3d9.upgradeBackBufferTo             = rgba16_sfloat
d3d9.enableSwapChainUpgrade          = true
d3d9.upgradeSwapChainFormatTo        = rgba16_sfloat
d3d9.upgradeSwapChainColorSpaceTo    = scRGB
```

Notes:
- Render target upgrades preserve "overbright" data that would otherwise clip. They require the swapchain upgrade too, or that data is lost again at present.
- **TYPELESS formats must match their bitness siblings.** Upgrading `RGBA8_UNORM` → `RGBA16_SFLOAT` requires upgrading `RGBA8_TYPELESS` → `RGBA16_TYPELESS`. TYPELESS formats cause the most visual breakage.
- There is a logging option that dumps the render target formats the app actually uses — use it, then upgrade selectively rather than blanket-upgrading.
- **DXVK performs no tone mapping.** That is Piece B's job.

**Known gap:** DXVK covers D3D8/9/10/11 only. No vkd3d-proton equivalent exists for D3D12. That is a separate, large project — out of scope.

### Piece B — the tone mapper (BUILD THIS)

A **Vulkan implicit layer** performing SDR→HDR inverse tone mapping as a compute pass at present time.

Why this shape solves every failure mode above:

- **No injection.** An ELF `.so` loaded by the Vulkan loader via a JSON manifest. No PE hooking, no CBT hooks, no SEH, no Win32. Sits *below* DXVK, not beside it.
- **No present-queue interference.** Frame timing is never touched. One compute dispatch on an already-resident image. The input-lag failure class is structurally impossible.
- **No extra copy** if designed correctly: app renders into a shadow image, the shader writes into the real swapchain image — same copy count the app already had.
- **Universal.** D3D9, D3D11, D3D12, and native Vulkan all bottom out at `vkQueuePresentKHR`. One code path.
- **Runs unchanged on Steam Deck OLED.** Same x86-64 binary, same RADV driver.

**Piece B works standalone without Piece A.** That is the MVP, and it covers D3D12 and native Vulkan which Piece A cannot. Piece A later becomes the quality upgrade for D3D9/11 titles.

---

## 3. Structural advantage over ReShade

Worth stating explicitly because it justifies the whole approach:

**ReShade sits above the graphics API and must ask the user what the buffer format and colorspace are. The layer creates the swapchain and therefore already knows.**

The layer has direct access to the exact `VkFormat`, the exact `VkColorSpaceKHR`, the full `VkSurfaceCapabilitiesKHR`, and whether Lilium's DXVK has already upgraded things to fp16. The single largest time sink in ReShade tuning — determining whether you are feeding the shader sRGB, 2.2-gamma, scRGB fp16, or HDR10 PQ, where a wrong guess produces plausible-but-wrong output diagnosable only by eye after a restart — does not exist here.

---

## 4. Hard requirements

Derived directly from the 2–3 min vs 2–3 hr gap. **Each of these will be tempting to violate during implementation. Don't.**

### R1 — Never ask the user what the source format is
Auto-detect from swapchain creation info. No config key for "input colorspace." If detection is ambiguous, measure (see R5) or pick a default and expose an override behind `advanced`.

### R2 — Zero restarts, ever, for anything
No preprocessor-define-driven shader variants. Compile all SPIR-V variants at layer load; switch via specialization constants and push constants at runtime. **If a knob requires a game restart, the knob is designed wrong.**

### R3 — One fused compute pass. No shader chain, no ordering, no plugins
The existence of hand-maintained "HDR shader order" guides in the ReShade ecosystem is proof that chaining is a broken UX. Everything inlines into a single `.comp`.

This will be tempting to violate the first time someone requests CAS or a LUT. **Say no** — vkBasalt exists and can run alongside.

### R4 — Five knobs in the default surface
1. Peak nits (display)
2. Paper white
3. Expansion strength
4. Saturation
5. Black floor

Everything else lives behind `advanced = true` in config. Special K exposes roughly this many; Pumbo + Lilium expose dozens spread across three interacting shaders. That difference is most of the 2-hour gap.

### R5 — Ship the HDR analysis pass in v0.1, not v1.0
Luminance histogram + running peak-nits readout, exposed live in the tuning UI. ~200 lines of compute shader. Without measurement the user is eyeballing highlights and re-tuning after every scene change — this is half of why SK feels fast.

### R6 — Tuning UI is a localhost web page, not an in-game overlay
A Vulkan layer cannot grab hotkeys on Wayland. **Lean into this — the resulting design is better than SK's:**

- Game stays **full-screen and unobstructed** while tuning. SK's ImGui panel covers exactly the thing you're trying to judge.
- On **Steam Deck OLED**: tune from a phone while looking at the handheld screen. No in-game overlay is usable on a 7" panel.
- On the **Steam Machine**: tune from a laptop while the TV shows the game.
- No Wayland input plumbing, no `/dev/input` permissions, no evdev, no compositor cooperation. Entire problem class eliminated.

Implementation: tiny embedded HTTP server + WebSocket pushing values into a uniform buffer read next frame. Single-header (`mongoose`, `civetweb`) or a hand-rolled ~300-line handler.

Keep TOML + `inotify` hot-reload as the headless/scriptable path, but **the browser UI is the primary surface.**

---

## 5. North star: auto-calibration

The analysis pass from R5 exists anyway. Take the next step.

Run the luminance histogram over the first few seconds of gameplay. Fit expansion parameters against actual content distribution — where the highlight cluster sits, where mid-tones land, whether a hard white clip at 1.0 suggests an already-tonemapped source, whether the gamut is already wide. Converge, then hold.

Per-game presets become a *starting hint* rather than something a human must author.

Nobody has built this. SK requires 2–3 minutes of human judgment per game. RenoDX requires a shader author to reverse-engineer each game's post chain. If auto-tune reaches "good" in five seconds and the five knobs take it from good to perfect in thirty, this is a genuinely new tool rather than a Linux port of an existing idea.

**Risk — this is the part most likely to fail.** SDR-vs-2.2-gamma ambiguity is real. Content-adaptive expansion will pump and breathe across scene cuts without careful temporal hysteresis. Build it behind a flag; manual presets remain the guaranteed path.

---

## 6. Known hard problem: UI/HUD blowout

**This is the real quality ceiling, and it is why gamescope's ITM looks mediocre.**

Special K composites the game's UI separately and excludes it from expansion. A Vulkan layer sees one flat image and cannot distinguish HUD from world.

Mitigations are shader-side, not architectural:
- Hue-conserving, luminance-only expansion
- Rolloff that leaves near-white largely untouched (Pumbo's design philosophy is explicitly additive enhancement rather than dramatic remapping)
- Gamut compression

This is craft work and it is where most of the project's time will go. It is a shader design problem, not a reason to change the architecture.

**Corollary: per-game presets are the product.** SK and RenoDX are not better because of better code — they are better because someone tuned each game. Design the preset database and the community contribution path early, or the result looks like gamescope ITM.

---

## 7. Prior art — read before writing code

| Project | License | Take from it |
|---|---|---|
| **vkBasalt** — `github.com/DadSchoorse/vkBasalt` | zlib | **Primary architectural template.** Layer scaffolding, dispatch table setup, present hook, effect dispatch. Meson build. |
| **low_latency_layer** — `github.com/Korthos-Software/low_latency_layer` | MIT | Modern C++23 layer example. **Also solves the Reflex/frame-pacing half of the original goal for free — do not rebuild it.** Implements `VK_NV_low_latency2` and `VK_AMD_anti_lag` vendor-agnostically; pairs with dxvk-nvapi for Proton games. |
| **VK_hdr_layer** — `github.com/Zamundaaa/VK_hdr_layer` | — | WSI colorspace plumbing to the compositor. Note: not for use with gamescope, which implements the color management protocols directly. |
| **gamescope** ITM shaders | BSD-ish | Reference for the naive version and the bar to beat. |
| **Lilium's ReShade_HDR_shaders** — `github.com/EndlesslyFlowering/ReShade_HDR_shaders` | — | Inverse tone mapping math, HLSL. Port to GLSL. |
| **PumboAutoHDR** — `github.com/Filoppi/PumboAutoHDR` | — | ITM math with a restrained, additive design philosophy. |
| **RenoDX** | MIT | Not portable (rides ReShade's add-on system). Its per-game tuning parameters are the best available documentation of what actually matters. |

### License strategy — important

**Ship MIT.** Matches low_latency_layer, compatible with vkBasalt (zlib) if scaffolding is borrowed, and permits Valve or the Bazzite maintainers to vendor it if it becomes good.

**Do not copy anything from Special K.** It is GPLv3. A single copied shader makes this unmergeable into DXVK (zlib) or gamescope permanently.

**Build system: Meson.** Matches DXVK, vkBasalt, and gamescope — Bazzite and Arch packagers will already know how to package it.

---

## 8. Repo skeleton

```
vkhdr/
├── meson.build
├── LICENSE                    # MIT
├── README.md
├── src/
│   ├── layer.cpp              # vkGetInstanceProcAddr chain, dispatch tables
│   ├── swapchain.cpp          # format substitution + shadow images
│   ├── present.cpp            # vkQueuePresentKHR hook, compute dispatch
│   ├── analysis.cpp           # histogram readback, peak-nits tracking
│   ├── config.cpp             # TOML + inotify hot-reload
│   ├── httpd.cpp              # localhost tuning UI + WebSocket
│   └── shaders/
│       ├── itm.comp           # THE fused pass — one shader, no chain
│       └── analyze.comp
├── layer/
│   └── vkhdr.json             # implicit layer manifest
├── presets/
│   └── *.toml                 # per-exe presets; community PRs land here
└── ui/
    └── index.html             # single-file slider page, no build step
```

Target activation surface — nothing more than this:

```bash
ENABLE_VKHDR=1 VKHDR_PRESET=dishonored2 %command%
# tuning UI at http://<host>:7331
```

---

## 9. Sprint plan

### Sprint 0 — baseline (do this first, ~10 minutes)

Establish what has to be beaten. On the Deck OLED in desktop mode:

```bash
gamescope -f --hdr-enabled --hdr-itm-enable \
  --hdr-itm-sdr-nits 100 --hdr-itm-target-nits 800 \
  -W 1280 -H 800 -- %command%
```

(`--hdr-itm-enable` is SDR-input only. `--hdr-itm-sdr-nits` default 100 / max 1000; `--hdr-itm-target-nits` default 1000 / max 10000.)

**If gamescope's ITM lands at ~70% of acceptable, the project scope shrinks to "better shaders + presets," which is much smaller than "write a layer." Find out before committing.**

### Sprint 1 — layer skeleton + format substitution (a weekend)

**Target a native Vulkan game first, NOT through Proton.** Removes DXVK from the variable list while debugging swapchain substitution. Add Proton once the layer is known-good.

Brief for Claude Code (clone `vkBasalt/` alongside as reference):

> Build a Vulkan implicit layer in C++20 with Meson. Intercept `vkCreateSwapchainKHR`; when the app requests an 8-bit UNORM/sRGB format and the surface supports `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`, create the real swapchain as `VK_FORMAT_R16G16B16A16_SFLOAT` in that colorspace, and allocate shadow images in the app's originally requested format. Return the shadow images from `vkGetSwapchainImagesKHR` so the app renders unmodified. Hook `vkQueuePresentKHR` to dispatch a compute shader reading the shadow image and writing the real swapchain image; for sprint 1 the shader is a straight linearize-and-scale with a hardcoded paper white of 200 nits. Log the detected source format, colorspace, and surface capabilities to stderr on swapchain creation. Reference vkBasalt's layer chaining and dispatch table setup in `src/`. Do not add an effect chain abstraction — a single pass only.

**Success condition: HDR engages and the image is not broken.** Binary, verifiable.

### Sprint 2 — config, hot-reload, real ITM math

- TOML config + `inotify` hot-reload
- Port Lilium's ITM math from HLSL to GLSL
- Analysis compute pass (histogram, peak nits)
- Tune against a real game on the OLED panel

### Sprint 3 — presets, UI, Proton

- Per-exe preset lookup + preset database structure
- localhost HTTP/WebSocket tuning UI
- Deck launch-option integration
- Pair with Lilium's DXVK fork on a D3D11 title; measure what the render target upgrade actually buys

### Later

- Auto-calibration behind a flag (Section 5)
- Wayland color-management-v1 query for display peak luminance / HDR metadata

---

## 10. Division of labor

**Claude Code is well-suited to:**
- Sprint 1 in full — Vulkan layer boilerplate, dispatch tables, swapchain interposition. Heavy boilerplate with a clear reference implementation to imitate.
- HLSL → GLSL shader math translation (self-contained, mechanical).
- The HTTP/WebSocket server (fixed-format protocol).
- Meson/CMake plumbing, preset file schema, packaging.
- Reading vkBasalt and low_latency_layer and explaining their present-hook patterns.

**Human-only:**
- All tuning. Whether the highlight rolloff looks right on the panel is not a testable proposition for a model.
- Driver/compositor behavioral debugging ("why does this present path stutter only under KWin").
- Judging auto-calibration convergence quality.

---

## 11. Verification


```bash
# Target capabilities
vulkaninfo | grep -iE 'hdr_metadata|swapchain_colorspace|EXTENDED_SRGB|A16B16G16R16'

# Reference implementations
git clone https://github.com/DadSchoorse/vkBasalt
git clone https://github.com/Korthos-Software/low_latency_layer
git clone https://github.com/Zamundaaa/VK_hdr_layer
git clone -b HDR-mod https://github.com/EndlesslyFlowering/dxvk
git clone https://github.com/EndlesslyFlowering/ReShade_HDR_shaders
git clone https://github.com/Filoppi/PumboAutoHDR
```

---

## 12. Open questions

1. **Name.** Decide before first commit.
2. **Does gamescope ITM already clear 70%?** Sprint 0 answers this and may reshape the project.
3. **Shadow-image passthrough correctness.** Does returning non-swapchain images from `vkGetSwapchainImagesKHR` hold up across all apps, or do some paths (image acquisition semantics, `VK_KHR_swapchain_maintenance1`, present modes) require special handling? Verify early in Sprint 1.
4. **SDR vs 2.2-gamma disambiguation.** Can this be measured reliably from a histogram, or does it need a per-game preset field?
5. **HDR metadata source.** Wayland color-management-v1 for display peak luminance vs. asking the user. Prefer querying.
6. **D3D12 precision gap.** Piece A has no vkd3d-proton equivalent. Accept the quality ceiling for D3D12 titles, or scope a follow-on project?
