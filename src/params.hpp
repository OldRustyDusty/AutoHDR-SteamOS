// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace hdrket {

// Source transfer function: how the game's 8/10-bit values map to light.
enum class Eotf : uint32_t { SRGB = 0, Gamma22 = 1, Gamma24 = 2, Linear = 3 };

// What happens to light above the display's peak.
//   Off:      sent as is; the display or compositor deals with it
//   Clip:     hue-preserving clamp at peak
//   Map:      BT.2390 roll-off from a fixed source peak into the display peak
//   Adaptive: same roll-off, source peak follows the measured scene peak
enum class Tonemap : uint32_t { Off = 0, Clip = 1, Map = 2, Adaptive = 3 };

// What the user tunes. Every field applies on the next frame (requirement R2).
struct Settings {
    float peakNits      = 1000.0f; // display peak
    float greyNits      = 36.0f;   // where SDR middle grey (18%) lands; sets overall brightness
    float expansion     = 0.5f;    // 0 = SDR in an HDR container, 1 = SDR white reaches peak, >1 = beyond (tonemapped)
    float saturation    = 1.0f;    // 1 = unchanged
    float blackFloor    = 0.0f;    // SDR-relative level crushed to black (fixes raised blacks)
    Eotf  eotf          = Eotf::Gamma22;
    Tonemap tonemap       = Tonemap::Adaptive;
    float gamutExpansion  = 0.0f;  // 0 = accurate BT.709, 1 = saturated colors stretched to BT.2020
    // Advanced
    float shoulderStart = 0.5f;    // SDR luminance where expansion begins
    float shoulderPow   = 2.5f;    // curve shape above the shoulder
    float adaptRelease  = 2.0f;    // adaptive tonemap: seconds to relax after a bright scene
    // Comparison and calibration aids
    float split   = 0.0f;          // left fraction of the screen shows the SDR reference
    bool  pattern = false;         // brightness step bars across the top
    bool  bypass  = false;         // whole screen as SDR reference

    bool  peakOverride = false;    // peak is this game's own (on this display), not the display's

    float paperWhiteNits() const { return greyNits / 0.18f; }
};

// Push constant block for itm.comp. Layout must match `Params` in
// shaders/common.glsl.
struct ItmParams {
    float    peakNits;
    float    paperWhiteNits;
    float    expansion;
    float    saturation;
    float    blackFloor;
    float    shoulderStart;
    float    shoulderPow;
    float    splitX;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t eotf;
    uint32_t tonemap;
    float    gamutExpansion;
    float    sourcePeakNits; // tone map input range; filled per frame by present.cpp
    float    pad;
};
static_assert(sizeof(ItmParams) == 64);

enum ItmFlags : uint32_t {
    ITM_BYPASS  = 1u << 1,
    ITM_PATTERN = 1u << 2,
};

ItmParams toPushConstants(const Settings& s);

// Preset/UI keys, e.g. ("peak_nits", "600") or ("eotf", "gamma24").
bool        settingsApplyKey(Settings& s, const std::string& key, const std::string& value);
const char* settingsEotfName(Eotf e);
const char* settingsTonemapName(Tonemap t);

// The brightest SDR white can become after expansion, in nits.
float expansionTargetNits(const Settings& s);

// Live settings shared between the render thread and the tuning UI.
Settings currentSettings();
void     updateSettings(const Settings& s);
uint64_t settingsVersion(); // bumps on every change

// Per-game presets: ~/.config/hdrket/presets/<game>.conf (picture settings).
// Per-display profiles: ~/.config/hdrket/displays/<display>.conf (peak).
const std::string& gameKey();
bool               savePreset(std::string* pathOut);

struct DisplayInfo {
    std::string key;       // profile file name: <name>-<edid crc>
    std::string name;      // EDID monitor name, e.g. "LG TV SSCR2"
    float       edidPeak = 0; // from the CTA HDR static metadata block, 0 if absent
};
const DisplayInfo& displayInfo();
void               loadInitialSettings(); // defaults -> preset -> HDRKET_* env

// Content statistics from the analysis pass, for the UI (requirement R5).
constexpr int kHistogramBins = 64;
constexpr float kHistogramMinLog2 = -2.0f; // 0.25 nits
constexpr float kHistogramMaxLog2 = 14.0f; // 16384 nits

struct Stats {
    float frameAvgNits  = 0;  // average light level of the last frame
    float framePeakNits = 0;  // brightest sampled pixel, last frame
    float p99Nits       = 0;  // 99th percentile, last frame
    float heldPeakNits  = 0;  // running peak, decays slowly
    float contentPeakNits = 0; // brightest sample before tone mapping, last frame
    float tonemapSourceNits = 0; // range the tone mapper is compressing from (0 = not mapping)
    std::array<float, kHistogramBins> histogram{}; // fraction of pixels per bin
    uint64_t frames = 0;
};

struct SwapchainInfo {
    std::string outputFormat;
    std::string outputColorSpace;
    std::string sourceFormat;
    uint32_t    width = 0, height = 0;
};

void  publishStats(const Stats& s);
Stats latestStats();
void  publishSwapchainInfo(const SwapchainInfo& info);
SwapchainInfo latestSwapchainInfo();

// httpd.cpp
void startTuningServer(); // idempotent; called once an HDR swapchain exists

} // namespace hdrket
