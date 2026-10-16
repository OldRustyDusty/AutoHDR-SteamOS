// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace hdrket {

// Push constant block for itm.comp. Layout must match `Params` in
// shaders/common.glsl. Every knob lives here so changes apply next frame.
struct ItmParams {
    float    peakNits       = 1000.0f; // display peak
    float    paperWhiteNits = 200.0f;  // where SDR white (1.0) lands
    float    expansion      = 0.5f;    // 0 = plain SDR in HDR container, 1 = SDR white reaches peak
    float    saturation     = 1.0f;    // 1 = neutral
    float    blackFloor     = 0.0f;    // SDR-relative level crushed to black (fixes raised blacks)
    float    shoulderStart  = 0.5f;    // advanced: SDR luminance where expansion starts
    float    shoulderPow    = 2.5f;    // advanced: curve shape above the shoulder
    float    splitX         = 0.0f;    // compare: left fraction of screen shows SDR reference
    uint32_t flags          = 0;
    uint32_t width          = 0;
    uint32_t height         = 0;
    float    pad            = 0.0f;
};
static_assert(sizeof(ItmParams) == 48);

enum ItmFlags : uint32_t {
    ITM_SRGB_DECODE = 1u << 0, // piecewise sRGB EOTF instead of gamma 2.2
    ITM_BYPASS      = 1u << 1, // linearize-and-scale only
};

// Sprint 1: defaults with env overrides. Replaced by config + live UI.
ItmParams currentParams();

} // namespace hdrket
