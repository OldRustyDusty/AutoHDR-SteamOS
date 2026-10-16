// SPDX-License-Identifier: MIT
// Shared definitions for HDR Ket compute shaders.

#define FLAG_BYPASS  2u
#define FLAG_PATTERN 4u

#define EOTF_SRGB    0u
#define EOTF_GAMMA22 1u
#define EOTF_GAMMA24 2u
#define EOTF_LINEAR  3u

#define TONEMAP_OFF      0u
#define TONEMAP_CLIP     1u
#define TONEMAP_MAP      2u
#define TONEMAP_ADAPTIVE 3u

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
    uint  eotf;
    uint  tonemap;
    float gamutExpansion;
    float sourcePeakNits;
    float pad;
} p;

// Must match kHistogram* in params.hpp.
#define HIST_BINS     64
#define HIST_MIN_LOG2 -2.0
#define HIST_MAX_LOG2 14.0

const float SCRGB_REFERENCE_NITS = 80.0;  // scRGB 1.0
const float PQ_MAX_NITS          = 10000.0;

float luminance709(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Most PC games are authored on gamma 2.2 displays, so 2.2 is the default.
// The piecewise sRGB curve lifts near-black.
vec3 decodeSRGB(vec3 e)
{
    vec3 lo = e / 12.92;
    vec3 hi = pow((max(e, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(e, vec3(0.04045)));
}

vec3 decodeSource(vec3 e, uint eotf)
{
    if (eotf == EOTF_SRGB)
        return decodeSRGB(e);
    if (eotf == EOTF_GAMMA24)
        return pow(max(e, vec3(0.0)), vec3(2.4));
    if (eotf == EOTF_LINEAR)
        return e;
    return pow(max(e, vec3(0.0)), vec3(2.2));
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

vec3 bt2020ToBt709(vec3 c)
{
    const mat3 m = mat3(
         1.6604910, -0.1245505, -0.0181508,
        -0.5876411,  1.1328999, -0.1005789,
        -0.0728499, -0.0083494,  1.1187297);
    return m * c;
}

float luminance2020(vec3 c)
{
    return dot(c, vec3(0.2627, 0.6780, 0.0593));
}

float max3(vec3 c)
{
    return max(c.r, max(c.g, c.b));
}

float min3(vec3 c)
{
    return min(c.r, min(c.g, c.b));
}

const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;

float pqEncode1(float nits)
{
    float ym = pow(clamp(nits / PQ_MAX_NITS, 0.0, 1.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2);
}

float pqDecode1(float e)
{
    float ep = pow(clamp(e, 0.0, 1.0), 1.0 / PQ_M2);
    return PQ_MAX_NITS * pow(max(ep - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * ep), 1.0 / PQ_M1);
}

// ITU-R BT.2390 EETF (section 5.4): maps [0, srcPeak] into [0, dstPeak] with
// a Hermite roll-off in PQ space. Below the knee, values pass unchanged.
float bt2390Eetf(float nits, float srcPeak, float dstPeak)
{
    if (srcPeak <= dstPeak)
        return nits;
    float pqSrc  = pqEncode1(srcPeak);
    float e1     = pqEncode1(nits) / pqSrc;
    float maxLum = pqEncode1(dstPeak) / pqSrc;
    float ks     = max(1.5 * maxLum - 0.5, 0.0);
    if (e1 <= ks)
        return nits;
    float t  = (min(e1, 1.0) - ks) / (1.0 - ks);
    float t2 = t * t;
    float t3 = t2 * t;
    float e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * maxLum;
    return pqDecode1(e2 * pqSrc);
}

// Gamut expansion: blend the colorimetric BT.709 -> BT.2020 conversion with
// the same RGB values read as BT.2020 primaries, which saturates them. The
// blend is weighted by the pixel's chroma, so greys are untouched (both paths
// agree on white) and low-chroma tones like skin barely move. Luminance is
// restored afterwards so only colorfulness changes.
vec3 expandGamut(vec3 c709, float amount)
{
    vec3 accurate = bt709ToBt2020(c709);
    if (amount <= 0.0)
        return accurate;
    float mx     = max3(c709);
    float chroma = mx > 0.0 ? clamp((mx - max(min3(c709), 0.0)) / mx, 0.0, 1.0) : 0.0;
    float w      = amount * smoothstep(0.15, 0.85, chroma);
    vec3  result = mix(accurate, max(c709, vec3(0.0)), w);
    float yOut   = luminance2020(result);
    if (yOut > 1e-6)
        result *= luminance2020(accurate) / yOut;
    return result;
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
