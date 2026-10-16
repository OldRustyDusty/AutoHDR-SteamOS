// SPDX-License-Identifier: MIT
#include "hdrket.hpp"

namespace hdrket {

bool isSdrSourceFormat(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
        return true;
    default:
        return false;
    }
}

bool isSrgbFormat(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

VkFormat unormViewFormat(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default:
        return f;
    }
}

const char* formatName(VkFormat f)
{
    switch (f) {
#define F(x) \
    case VK_FORMAT_##x: return #x;
        F(UNDEFINED)
        F(B8G8R8A8_UNORM)
        F(B8G8R8A8_SRGB)
        F(R8G8B8A8_UNORM)
        F(R8G8B8A8_SRGB)
        F(A8B8G8R8_UNORM_PACK32)
        F(A8B8G8R8_SRGB_PACK32)
        F(A2B10G10R10_UNORM_PACK32)
        F(A2R10G10B10_UNORM_PACK32)
        F(R16G16B16A16_UNORM)
        F(R16G16B16A16_SFLOAT)
        F(R5G6B5_UNORM_PACK16)
        F(B5G6R5_UNORM_PACK16)
        F(B10G11R11_UFLOAT_PACK32)
#undef F
    default:
        return "other";
    }
}

const char* colorSpaceName(VkColorSpaceKHR cs)
{
    switch (cs) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "EXTENDED_SRGB_LINEAR (scRGB)";
    case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return "EXTENDED_SRGB_NONLINEAR";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084 (PQ)";
    case VK_COLOR_SPACE_HDR10_HLG_EXT: return "HDR10_HLG";
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return "BT2020_LINEAR";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "DISPLAY_P3_NONLINEAR";
    case VK_COLOR_SPACE_PASS_THROUGH_EXT: return "PASS_THROUGH";
    default: return "other";
    }
}

} // namespace hdrket
