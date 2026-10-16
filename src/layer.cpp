// SPDX-License-Identifier: MIT
//
// Layer plumbing: instance/device creation, dispatch tables, GetProcAddr.
// The chaining pattern follows vkBasalt (zlib) and the Vulkan loader's
// layer interface documentation.

#include "hdrket.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace hdrket {

Globals& globals()
{
    static Globals g;
    return g;
}

InstanceData* findInstance(void* key)
{
    auto& g  = globals();
    auto  it = g.instances.find(key);
    return it == g.instances.end() ? nullptr : it->second.get();
}

DeviceData* findDevice(void* key)
{
    auto& g  = globals();
    auto  it = g.devices.find(key);
    return it == g.devices.end() ? nullptr : it->second.get();
}

static bool hasExtension(const std::vector<const char*>& list, const char* name)
{
    return std::any_of(list.begin(), list.end(), [&](const char* e) { return !std::strcmp(e, name); });
}

template <typename Info, typename LinkInfo>
static Info* findChainInfo(const void* pNext, VkStructureType sType, VkLayerFunction function)
{
    auto* info = static_cast<Info*>(const_cast<void*>(pNext));
    while (info && !(info->sType == sType && info->function == function))
        info = static_cast<Info*>(const_cast<void*>(info->pNext));
    return info;
}

// ---------------------------------------------------------------------------
// Instance

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                          VkInstance* pInstance)
{
    auto* chain = findChainInfo<VkLayerInstanceCreateInfo, VkLayerInstanceLink>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo            = chain->u.pLayerInfo->pNext;

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));

    // HDR colorspaces need VK_EXT_swapchain_colorspace, which an SDR app
    // never enables. Add it; if the driver lacks it, fall back to the app's list.
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    bool added = false;
    if (!hasExtension(exts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        added = true;
    }

    VkInstanceCreateInfo info    = *pCreateInfo;
    info.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    info.ppEnabledExtensionNames = exts.data();

    VkResult res = createInstance(&info, pAllocator, pInstance);
    bool colorspace = res == VK_SUCCESS;
    if (res == VK_ERROR_EXTENSION_NOT_PRESENT && added) {
        HK_WARN("driver lacks %s; HDR output unavailable", VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        res        = createInstance(pCreateInfo, pAllocator, pInstance);
        colorspace = false;
    }
    if (res != VK_SUCCESS)
        return res;

    auto data                 = std::make_unique<InstanceData>();
    data->instance            = *pInstance;
    data->swapchainColorspace = colorspace;
#define LOAD(name) data->vk.name = reinterpret_cast<PFN_vk##name>(gipa(*pInstance, "vk" #name));
    HDRKET_INSTANCE_FUNCS(LOAD)
#undef LOAD

    const char* app = pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName
                          ? pCreateInfo->pApplicationInfo->pApplicationName
                          : "?";
    const char* engine = pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pEngineName
                             ? pCreateInfo->pApplicationInfo->pEngineName
                             : "?";
    HK_INFO("v%s loaded (app \"%s\", engine \"%s\")", HDRKET_VERSION, app, engine);

    std::lock_guard l(globals().lock);
    globals().instances[dispatchKey(*pInstance)] = std::move(data);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    if (!instance)
        return;
    std::unique_ptr<InstanceData> data;
    {
        std::lock_guard l(globals().lock);
        auto it = globals().instances.find(dispatchKey(instance));
        if (it == globals().instances.end())
            return;
        data = std::move(it->second);
        globals().instances.erase(it);
    }
    data->vk.DestroyInstance(instance, pAllocator);
}

// ---------------------------------------------------------------------------
// Device

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    auto* chain = findChainInfo<VkLayerDeviceCreateInfo, VkLayerDeviceLink>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VkLayerDeviceLink>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LOADER_DATA_CALLBACK);
    if (!chain || !loaderData)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo            = chain->u.pLayerInfo->pNext;

    InstanceData* inst;
    {
        std::lock_guard l(globals().lock);
        inst = findInstance(dispatchKey(physicalDevice));
    }
    if (!inst)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));

    // VK_EXT_hdr_metadata lets the compositor know our real peak, so it
    // doesn't tone map the output a second time.
    uint32_t count = 0;
    inst->vk.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    inst->vk.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
    bool canHdrMetadata = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
        return !std::strcmp(e.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    });

    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    if (canHdrMetadata && !hasExtension(exts, VK_EXT_HDR_METADATA_EXTENSION_NAME))
        exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);

    VkDeviceCreateInfo info      = *pCreateInfo;
    info.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    info.ppEnabledExtensionNames = exts.data();

    VkResult res = createDevice(physicalDevice, &info, pAllocator, pDevice);
    if (res != VK_SUCCESS)
        return res;

    auto dev            = std::make_unique<DeviceData>();
    dev->device         = *pDevice;
    dev->physicalDevice = physicalDevice;
    dev->instance       = inst;
    dev->setLoaderData  = loaderData->u.pfnSetDeviceLoaderData;
    dev->hdrMetadata    = canHdrMetadata;
#define LOAD(name) dev->vk.name = reinterpret_cast<PFN_vk##name>(gdpa(*pDevice, "vk" #name));
    HDRKET_DEVICE_FUNCS(LOAD)
#undef LOAD

    uint32_t familyCount = 0;
    inst->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    dev->families.resize(familyCount);
    inst->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, dev->families.data());
    inst->vk.GetPhysicalDeviceMemoryProperties(physicalDevice, &dev->memoryProperties);

    // Remember which family every queue belongs to: present can happen on any
    // of them, and our pass is recorded for the presenting queue's family.
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        const auto& q = pCreateInfo->pQueueCreateInfos[i];
        if (q.flags != 0)
            continue; // protected queues need vkGetDeviceQueue2; we never present on them
        for (uint32_t j = 0; j < q.queueCount; j++) {
            VkQueue queue = VK_NULL_HANDLE;
            dev->vk.GetDeviceQueue(*pDevice, q.queueFamilyIndex, j, &queue);
            if (queue)
                dev->queueFamily[queue] = q.queueFamilyIndex;
        }
    }

    VkPhysicalDeviceProperties props;
    inst->vk.GetPhysicalDeviceProperties(physicalDevice, &props);
    HK_INFO("device: %s (hdr_metadata %s, colorspace ext %s)", props.deviceName, canHdrMetadata ? "yes" : "no",
            inst->swapchainColorspace ? "yes" : "no");

    std::lock_guard l(globals().lock);
    globals().devices[dispatchKey(*pDevice)] = std::move(dev);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    if (!device)
        return;
    std::unique_ptr<DeviceData> dev;
    {
        std::lock_guard l(globals().lock);
        auto it = globals().devices.find(dispatchKey(device));
        if (it == globals().devices.end())
            return;
        dev = std::move(it->second);
        globals().devices.erase(it);

        // Apps are supposed to destroy swapchains first; clean up if they didn't.
        for (auto sit = globals().swapchains.begin(); sit != globals().swapchains.end();) {
            if (sit->second->device == dev.get()) {
                releaseSwapchain(*sit->second);
                sit = globals().swapchains.erase(sit);
            }
            else
                ++sit;
        }
    }
    destroyDeviceResources(*dev);
    dev->vk.DestroyDevice(device, pAllocator);
}

// ---------------------------------------------------------------------------
// Enumeration (used when the layer is enabled explicitly)

static VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProps)
{
    if (!pProps) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1)
        return VK_INCOMPLETE;
    *pCount = 1;
    std::strncpy(pProps->layerName, HDRKET_LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE);
    std::strncpy(pProps->description, "HDR Ket: SDR to HDR inverse tone mapping", VK_MAX_DESCRIPTION_SIZE);
    pProps->implementationVersion = 1;
    pProps->specVersion           = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount, VkLayerProperties* pProps)
{
    return EnumerateInstanceLayerProperties(pCount, pProps);
}

static VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount,
                                                                VkExtensionProperties*)
{
    if (!pLayerName || std::strcmp(pLayerName, HDRKET_LAYER_NAME))
        return VK_ERROR_LAYER_NOT_PRESENT;
    *pCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
                                                              uint32_t* pCount, VkExtensionProperties* pProps)
{
    if (pLayerName && !std::strcmp(pLayerName, HDRKET_LAYER_NAME)) {
        *pCount = 0;
        return VK_SUCCESS;
    }
    InstanceData* inst;
    {
        std::lock_guard l(globals().lock);
        inst = findInstance(dispatchKey(physicalDevice));
    }
    if (!inst)
        return VK_ERROR_INITIALIZATION_FAILED;
    return inst->vk.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProps);
}

} // namespace hdrket

// ---------------------------------------------------------------------------
// Entry points

using namespace hdrket;

static PFN_vkVoidFunction interceptDevice(const char* name)
{
#define HOOK(fn) \
    if (!std::strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&hdrket::fn);
    HOOK(DestroyDevice)
    HOOK(CreateSwapchainKHR)
    HOOK(DestroySwapchainKHR)
    HOOK(GetSwapchainImagesKHR)
    HOOK(QueuePresentKHR)
    HOOK(EnumerateDeviceLayerProperties)
    HOOK(EnumerateDeviceExtensionProperties)
    return nullptr;
}

static PFN_vkVoidFunction interceptInstance(const char* name)
{
    HOOK(CreateInstance)
    HOOK(DestroyInstance)
    HOOK(CreateDevice)
    HOOK(EnumerateInstanceLayerProperties)
    HOOK(EnumerateInstanceExtensionProperties)
#undef HOOK
    return interceptDevice(name);
}

HDRKET_EXPORT PFN_vkVoidFunction VKAPI_CALL hdrket_GetDeviceProcAddr(VkDevice device, const char* pName);
HDRKET_EXPORT PFN_vkVoidFunction VKAPI_CALL hdrket_GetInstanceProcAddr(VkInstance instance, const char* pName);

HDRKET_EXPORT PFN_vkVoidFunction VKAPI_CALL hdrket_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (!std::strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&hdrket_GetDeviceProcAddr);
    if (auto fn = interceptDevice(pName))
        return fn;

    std::lock_guard l(globals().lock);
    DeviceData* dev = findDevice(dispatchKey(device));
    return dev ? dev->vk.GetDeviceProcAddr(device, pName) : nullptr;
}

HDRKET_EXPORT PFN_vkVoidFunction VKAPI_CALL hdrket_GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!std::strcmp(pName, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&hdrket_GetInstanceProcAddr);
    if (!std::strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&hdrket_GetDeviceProcAddr);
    if (auto fn = interceptInstance(pName))
        return fn;
    if (!instance)
        return nullptr;

    std::lock_guard l(globals().lock);
    InstanceData* inst = findInstance(dispatchKey(instance));
    return inst ? inst->vk.GetInstanceProcAddr(instance, pName) : nullptr;
}
