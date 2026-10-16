// SPDX-License-Identifier: MIT
// Shared definitions for HDR Ket compute shaders.

#define FLAG_SRGB_DECODE 1u
#define FLAG_BYPASS      2u

// Must match ItmParams in params.hpp.
layout(push_constant) uniform Params {
    float peakNits;
    float paperWhiteNits;
    float expansion;
    float saturation;
    float blackFloor;
    float shoulderStart;
    float shoulderPow;
    float splitX;
    uint  flags;
    uint  width;
    uint  height;
    float pad;
} p;

const float SCRGB_REFERENCE_NITS = 80.0;  // scRGB 1.0
const float PQ_MAX_NITS          = 10000.0;

float luminance709(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Most PC games are authored on gamma 2.2 displays, so 2.2 is the default.
// The piecewise sRGB curve lifts near-black; it's an advanced option.
vec3 decodeGamma22(vec3 e)
{
    return pow(max(e, vec3(0.0)), vec3(2.2));
}

vec3 decodeSRGB(vec3 e)
{
    vec3 lo = e / 12.92;
    vec3 hi = pow((max(e, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(e, vec3(0.04045)));
}

// ITU-R BT.2087 / BT.2020: linear BT.709 RGB to linear BT.2020 RGB.
vec3 bt709ToBt2020(vec3 c)
{
    const mat3 m = mat3(
        0.6274040, 0.0690970, 0.0163916,
        0.3292820, 0.9195400, 0.0880132,
        0.0433136, 0.0113612, 0.8955950);
    return m * c;
}

// SMPTE ST 2084 inverse EOTF, input in nits.
vec3 pqEncode(vec3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 y  = clamp(nits / PQ_MAX_NITS, 0.0, 1.0);
    vec3 ym = pow(y, vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}
