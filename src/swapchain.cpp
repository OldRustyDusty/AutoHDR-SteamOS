// SPDX-License-Identifier: MIT
//
// Swapchain interposition. For an SDR swapchain the real swapchain is created
// in an HDR format/colorspace, and the app is handed "shadow" images in the
// format it asked for. Image indices are shared 1:1, so acquire, release and
// present need no translation; only present does extra work (present.cpp).

#include "hdrket.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hdrket {

namespace {

enum class OutputPref { Auto, ScRGB, HDR10, Off };

OutputPref outputPreference()
{
    const char* env = std::getenv("HDRKET_OUTPUT");
    if (!env)
        return OutputPref::Auto;
    if (!std::strcmp(env, "scrgb"))
        return OutputPref::ScRGB;
    if (!std::strcmp(env, "hdr10"))
        return OutputPref::HDR10;
    if (!std::strcmp(env, "off"))
        return OutputPref::Off;
    return OutputPref::Auto;
}

struct OutputChoice {
    bool            ok = false;
    VkFormat        format{};
    VkColorSpaceKHR colorSpace{};
    OutputMode      mode{};
};

bool storageCapable(InstanceData& inst, VkPhysicalDevice pd, VkFormat f)
{
    VkFormatProperties props;
    inst.vk.GetPhysicalDeviceFormatProperties(pd, f, &props);
    return props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}

OutputChoice chooseOutput(DeviceData& dev, VkSurfaceKHR surface)
{
    InstanceData& inst = *dev.instance;

    VkSurfaceCapabilitiesKHR caps{};
    inst.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physicalDevice, surface, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
        HK_WARN("surface does not allow storage usage on swapchain images; passing through");
        return {};
    }

    uint32_t count = 0;
    inst.vk.GetPhysicalDeviceSurfaceFormatsKHR(dev.physicalDevice, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    inst.vk.GetPhysicalDeviceSurfaceFormatsKHR(dev.physicalDevice, surface, &count, formats.data());

    auto supported = [&](VkFormat f, VkColorSpaceKHR cs) {
        for (auto& sf : formats)
            if (sf.format == f && sf.colorSpace == cs)
                return storageCapable(inst, dev.physicalDevice, f);
        return false;
    };

    const OutputChoice scrgb{true, VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, OutputMode::ScRGB};
    const OutputChoice hdr10{true, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, OutputMode::HDR10};
    bool hasScRGB = supported(scrgb.format, scrgb.colorSpace);
    bool hasHDR10 = supported(hdr10.format, hdr10.colorSpace);

    HK_INFO("surface HDR formats: scRGB fp16 %s, HDR10 PQ 10-bit %s", hasScRGB ? "yes" : "no", hasHDR10 ? "yes" : "no");

    switch (outputPreference()) {
    case OutputPref::Off:
        return {};
    case OutputPref::ScRGB:
        if (hasScRGB)
            return scrgb;
        break;
    case OutputPref::HDR10:
        if (hasHDR10)
            return hdr10;
        break;
    case OutputPref::Auto:
        break;
    }
    // Default to fp16 scRGB: linear, full precision in the shadows, and no
    // gamut conversion needed for the 709 source. HDR10 halves the bandwidth.
    if (hasScRGB)
        return scrgb;
    if (hasHDR10)
        return hdr10;
    return {};
}

uint32_t findMemoryType(const DeviceData& dev, uint32_t typeBits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < dev.memoryProperties.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (dev.memoryProperties.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

void destroySlots(SwapchainData& sc)
{
    DeviceData& dev = *sc.device;
    auto&       vk  = dev.vk;
    for (auto& s : sc.slots)
        if (s.inFlight)
            vk.WaitForFences(dev.device, 1, &s.inFlight->fence, VK_TRUE, UINT64_MAX);
    for (auto& s : sc.slots) {
        s.inFlight.reset();
        if (s.cmd)
            vk.FreeCommandBuffers(dev.device, commandPoolFor(dev, s.cmdFamily), 1, &s.cmd);
        if (s.done)
            vk.DestroySemaphore(dev.device, s.done, nullptr);
        if (s.realView)
            vk.DestroyImageView(dev.device, s.realView, nullptr);
        if (s.shadowView)
            vk.DestroyImageView(dev.device, s.shadowView, nullptr);
        if (s.shadow)
            vk.DestroyImage(dev.device, s.shadow, nullptr);
        if (s.shadowMemory)
            vk.FreeMemory(dev.device, s.shadowMemory, nullptr);
    }
    sc.slots.clear();
    if (sc.descriptorPool)
        vk.DestroyDescriptorPool(dev.device, sc.descriptorPool, nullptr);
    sc.descriptorPool = VK_NULL_HANDLE;
    sc.imagesReady    = false;
}

void setHdrMetadata(SwapchainData& sc)
{
    DeviceData& dev = *sc.device;
    if (!dev.hdrMetadata || !dev.vk.SetHdrMetadataEXT)
        return;
    // Sprint 1: fixed values; later these follow the peak-nits knob.
    VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    if (sc.mode == OutputMode::HDR10) {
        md.displayPrimaryRed   = {0.708f, 0.292f};
        md.displayPrimaryGreen = {0.170f, 0.797f};
        md.displayPrimaryBlue  = {0.131f, 0.046f};
    } else {
        md.displayPrimaryRed   = {0.640f, 0.330f};
        md.displayPrimaryGreen = {0.300f, 0.600f};
        md.displayPrimaryBlue  = {0.150f, 0.060f};
    }
    md.whitePoint                = {0.3127f, 0.3290f};
    md.maxLuminance              = 1000.0f;
    md.minLuminance              = 0.0f;
    md.maxContentLightLevel      = 1000.0f;
    md.maxFrameAverageLightLevel = 200.0f;
    dev.vk.SetHdrMetadataEXT(dev.device, 1, &sc.handle, &md);
}

} // namespace

VkResult CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    std::lock_guard l(globals().lock);
    DeviceData* dev = findDevice(dispatchKey(device));
    if (!dev)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkSwapchainCreateInfoKHR& app = *pCreateInfo;
    HK_INFO("app swapchain: %ux%u %s / %s, %u images min, usage 0x%x, flags 0x%x", app.imageExtent.width,
            app.imageExtent.height, formatName(app.imageFormat), colorSpaceName(app.imageColorSpace), app.minImageCount,
            app.imageUsage, app.flags);

    const char* skip = nullptr;
    if (!dev->instance->swapchainColorspace)
        skip = "no VK_EXT_swapchain_colorspace";
    else if (app.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        skip = "app already outputs a non-SDR colorspace";
    else if (!isSdrSourceFormat(app.imageFormat))
        skip = "unhandled source format";
    else if (app.flags & (VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR | VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR))
        skip = "protected/split-instance swapchain";
    else if (app.presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
             app.presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR)
        skip = "shared presentable image";
    else if (app.imageArrayLayers != 1)
        skip = "layered (stereo) swapchain";

    OutputChoice out;
    if (!skip) {
        out = chooseOutput(*dev, app.surface);
        if (!out.ok)
            skip = "no usable HDR surface format";
    }
    if (skip) {
        HK_INFO("passthrough: %s", skip);
        return dev->vk.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    VkSwapchainCreateInfoKHR real = app;
    real.imageFormat     = out.format;
    real.imageColorSpace = out.colorSpace;
    real.imageUsage      = VK_IMAGE_USAGE_STORAGE_BIT;
    real.flags &= ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

    // A format list in the chain describes the app's formats and is only legal
    // with MUTABLE_FORMAT. Point it at our single format for the call.
    VkImageFormatListCreateInfo* formatList = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(app.pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            formatList = reinterpret_cast<VkImageFormatListCreateInfo*>(const_cast<VkBaseInStructure*>(p));
    VkImageFormatListCreateInfo savedList{};
    if (formatList) {
        savedList                  = *formatList;
        formatList->viewFormatCount = 1;
        formatList->pViewFormats    = &real.imageFormat;
    }

    VkResult res = dev->vk.CreateSwapchainKHR(device, &real, pAllocator, pSwapchain);

    if (formatList) {
        formatList->viewFormatCount = savedList.viewFormatCount;
        formatList->pViewFormats    = savedList.pViewFormats;
    }

    if (res != VK_SUCCESS) {
        HK_WARN("HDR swapchain creation failed (%d); passing through", res);
        return dev->vk.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    auto sc           = std::make_unique<SwapchainData>();
    sc->device        = dev;
    sc->handle        = *pSwapchain;
    sc->appInfo       = app;
    sc->appInfo.pNext = nullptr;
    sc->appFormat     = app.imageFormat;
    sc->outFormat     = out.format;
    sc->outColorSpace = out.colorSpace;
    sc->mode          = out.mode;
    sc->extent        = app.imageExtent;
    if (app.imageSharingMode == VK_SHARING_MODE_CONCURRENT && app.queueFamilyIndexCount) {
        sc->queueFamilies.assign(app.pQueueFamilyIndices, app.pQueueFamilyIndices + app.queueFamilyIndexCount);
        sc->appInfo.pQueueFamilyIndices = sc->queueFamilies.data();
    }
    sc->appMutable    = app.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    if (formatList && savedList.viewFormatCount)
        sc->appViewFormats.assign(savedList.pViewFormats, savedList.pViewFormats + savedList.viewFormatCount);

    HK_INFO("HDR swapchain: %s / %s replaces %s / %s", formatName(out.format), colorSpaceName(out.colorSpace),
            formatName(app.imageFormat), colorSpaceName(app.imageColorSpace));

    setHdrMetadata(*sc);
    globals().swapchains[*pSwapchain] = std::move(sc);
    return VK_SUCCESS;
}

static bool createSlots(SwapchainData& sc)
{
    DeviceData& dev = *sc.device;
    auto&       vk  = dev.vk;

    uint32_t count = 0;
    vk.GetSwapchainImagesKHR(dev.device, sc.handle, &count, nullptr);
    std::vector<VkImage> real(count);
    vk.GetSwapchainImagesKHR(dev.device, sc.handle, &count, real.data());
    sc.slots.resize(count);

    if (!ensurePipelines(dev))
        return false;

    // Shadow images: exactly what the app asked for, plus sampling for us.
    VkFormat viewFormat = unormViewFormat(sc.appFormat);
    std::vector<VkFormat> viewFormats = sc.appViewFormats;
    if (viewFormats.empty())
        viewFormats.push_back(sc.appFormat);
    if (viewFormat != sc.appFormat)
        viewFormats.push_back(viewFormat);

    VkImageFormatListCreateInfo listInfo{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    listInfo.viewFormatCount = static_cast<uint32_t>(viewFormats.size());
    listInfo.pViewFormats    = viewFormats.data();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = sc.appFormat;
    ici.extent      = {sc.extent.width, sc.extent.height, 1};
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = sc.appInfo.imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = sc.appInfo.imageSharingMode;
    ici.queueFamilyIndexCount = sc.appInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT ? sc.appInfo.queueFamilyIndexCount : 0;
    ici.pQueueFamilyIndices   = ici.queueFamilyIndexCount ? sc.appInfo.pQueueFamilyIndices : nullptr;
    ici.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;
    if (sc.appMutable || viewFormat != sc.appFormat) {
        ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ici.pNext = &listInfo;
    }

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets       = count;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = sizes;
    if (vk.CreateDescriptorPool(dev.device, &dpci, nullptr, &sc.descriptorPool) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        ImageSlot& s = sc.slots[i];
        s.real       = real[i];

        if (vk.CreateImage(dev.device, &ici, nullptr, &s.shadow) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req;
        vk.GetImageMemoryRequirements(dev.device, s.shadow, &req);

        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = s.shadow;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext           = &dedicated;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX)
            mai.memoryTypeIndex = findMemoryType(dev, req.memoryTypeBits, 0);
        if (vk.AllocateMemory(dev.device, &mai, nullptr, &s.shadowMemory) != VK_SUCCESS)
            return false;
        if (vk.BindImageMemory(dev.device, s.shadow, s.shadowMemory, 0) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vci.image            = s.shadow;
        vci.format           = viewFormat;
        if (vk.CreateImageView(dev.device, &vci, nullptr, &s.shadowView) != VK_SUCCESS)
            return false;
        vci.image  = s.real;
        vci.format = sc.outFormat;
        if (vk.CreateImageView(dev.device, &vci, nullptr, &s.realView) != VK_SUCCESS)
            return false;

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk.CreateSemaphore(dev.device, &sci, nullptr, &s.done) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = sc.descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &dev.setLayout;
        if (vk.AllocateDescriptorSets(dev.device, &dsai, &s.set) != VK_SUCCESS)
            return false;

        VkDescriptorImageInfo src{dev.sampler, s.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dst{VK_NULL_HANDLE, s.realView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet  writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = s.set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &src;
        writes[1]                 = writes[0];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &dst;
        vk.UpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
    }

    sc.imagesReady = true;
    HK_DBG("created %u shadow images (%s, view %s)", count, formatName(sc.appFormat), formatName(viewFormat));
    return true;
}

VkResult GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* pImages)
{
    std::lock_guard l(globals().lock);
    DeviceData* dev = findDevice(dispatchKey(device));
    if (!dev)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto it = globals().swapchains.find(swapchain);
    if (it == globals().swapchains.end())
        return dev->vk.GetSwapchainImagesKHR(device, swapchain, pCount, pImages);

    SwapchainData& sc = *it->second;
    if (!sc.imagesReady && !createSlots(sc)) {
        HK_ERR("failed to create shadow images");
        destroySlots(sc);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    uint32_t total = static_cast<uint32_t>(sc.slots.size());
    if (!pImages) {
        *pCount = total;
        return VK_SUCCESS;
    }
    uint32_t n = std::min(*pCount, total);
    for (uint32_t i = 0; i < n; i++)
        pImages[i] = sc.slots[i].shadow;
    *pCount = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

void DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    if (!swapchain)
        return;
    std::lock_guard l(globals().lock);
    DeviceData* dev = findDevice(dispatchKey(device));
    if (!dev)
        return;

    auto it = globals().swapchains.find(swapchain);
    if (it != globals().swapchains.end()) {
        destroySlots(*it->second);
        globals().swapchains.erase(it);
    }
    dev->vk.DestroySwapchainKHR(device, swapchain, pAllocator);
}

void releaseSwapchain(SwapchainData& sc)
{
    destroySlots(sc);
}

} // namespace hdrket
