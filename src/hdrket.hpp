// HDR Ket — SDR to HDR inverse tone mapping as a Vulkan implicit layer.
// SPDX-License-Identifier: MIT
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define HDRKET_LAYER_NAME "VK_LAYER_HDRKET_inverse_tonemap"

#define HDRKET_EXPORT extern "C" __attribute__((visibility("default")))

namespace hdrket {

// ---------------------------------------------------------------------------
// Dispatch tables. Only what the layer itself calls; everything else the app
// calls goes straight to the next layer via GetProcAddr passthrough.

#define HDRKET_INSTANCE_FUNCS(X)                  \
    X(GetInstanceProcAddr)                        \
    X(DestroyInstance)                            \
    X(EnumerateDeviceExtensionProperties)         \
    X(GetPhysicalDeviceProperties)                \
    X(GetPhysicalDeviceQueueFamilyProperties)     \
    X(GetPhysicalDeviceMemoryProperties)          \
    X(GetPhysicalDeviceFormatProperties)          \
    X(GetPhysicalDeviceSurfaceFormatsKHR)         \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)

#define HDRKET_DEVICE_FUNCS(X)      \
    X(GetDeviceProcAddr)            \
    X(DestroyDevice)                \
    X(GetDeviceQueue)               \
    X(CreateSwapchainKHR)           \
    X(DestroySwapchainKHR)          \
    X(GetSwapchainImagesKHR)        \
    X(QueuePresentKHR)              \
    X(CreateImage)                  \
    X(DestroyImage)                 \
    X(GetImageMemoryRequirements)   \
    X(AllocateMemory)               \
    X(FreeMemory)                   \
    X(BindImageMemory)              \
    X(CreateImageView)              \
    X(DestroyImageView)             \
    X(CreateSampler)                \
    X(DestroySampler)               \
    X(CreateDescriptorSetLayout)    \
    X(DestroyDescriptorSetLayout)   \
    X(CreatePipelineLayout)         \
    X(DestroyPipelineLayout)        \
    X(CreateShaderModule)           \
    X(DestroyShaderModule)          \
    X(CreateComputePipelines)       \
    X(DestroyPipeline)              \
    X(CreateDescriptorPool)         \
    X(DestroyDescriptorPool)        \
    X(AllocateDescriptorSets)       \
    X(UpdateDescriptorSets)         \
    X(CreateCommandPool)            \
    X(DestroyCommandPool)           \
    X(AllocateCommandBuffers)       \
    X(FreeCommandBuffers)           \
    X(BeginCommandBuffer)           \
    X(EndCommandBuffer)             \
    X(CmdPipelineBarrier)           \
    X(CmdBindPipeline)              \
    X(CmdBindDescriptorSets)        \
    X(CmdPushConstants)             \
    X(CmdDispatch)                  \
    X(CreateFence)                  \
    X(DestroyFence)                 \
    X(WaitForFences)                \
    X(ResetFences)                  \
    X(CreateSemaphore)              \
    X(DestroySemaphore)             \
    X(QueueSubmit)                  \
    X(QueueWaitIdle)                \
    X(DeviceWaitIdle)               \
    X(SetHdrMetadataEXT)

#define HDRKET_DECLARE_PFN(name) PFN_vk##name name = nullptr;

struct InstanceDispatch {
    HDRKET_INSTANCE_FUNCS(HDRKET_DECLARE_PFN)
};

struct DeviceDispatch {
    HDRKET_DEVICE_FUNCS(HDRKET_DECLARE_PFN)
};

#undef HDRKET_DECLARE_PFN

// ---------------------------------------------------------------------------
// Per-object state.

struct InstanceData {
    VkInstance       instance = VK_NULL_HANDLE;
    InstanceDispatch vk;
    bool             swapchainColorspace = false; // VK_EXT_swapchain_colorspace enabled
};

struct DeviceData;

// A fence shared by every swapchain image recorded in one present call. It
// returns to the device's free list once no image slot references it.
struct SubmitFence {
    DeviceData* device = nullptr;
    VkFence     fence  = VK_NULL_HANDLE;
};
using SubmitFenceRef = std::shared_ptr<SubmitFence>;

struct DeviceData {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    InstanceData*    instance       = nullptr;
    DeviceDispatch   vk;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;

    bool hdrMetadata = false; // VK_EXT_hdr_metadata enabled

    std::vector<VkQueueFamilyProperties>     families;
    VkPhysicalDeviceMemoryProperties         memoryProperties{};
    std::unordered_map<VkQueue, uint32_t>    queueFamily;
    std::unordered_map<uint32_t, VkCommandPool> commandPools; // by queue family

    // Shared by all swapchains on the device; created on first use.
    VkSampler             sampler        = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipelineScRGB  = VK_NULL_HANDLE;
    VkPipeline            pipelineHDR10  = VK_NULL_HANDLE;

    std::vector<VkFence> freeFences;
};

enum class OutputMode { ScRGB, HDR10 };

struct ImageSlot {
    VkImage        shadow       = VK_NULL_HANDLE; // what the app renders into
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    VkImageView    shadowView   = VK_NULL_HANDLE; // UNORM view: shader gets encoded values
    VkImage        real         = VK_NULL_HANDLE; // the presentable image
    VkImageView    realView     = VK_NULL_HANDLE;
    VkDescriptorSet set         = VK_NULL_HANDLE;
    VkCommandBuffer cmd         = VK_NULL_HANDLE;
    uint32_t        cmdFamily   = UINT32_MAX;
    VkSemaphore     done        = VK_NULL_HANDLE; // signalled by our pass, waited by present
    SubmitFenceRef  inFlight;                     // guards cmd re-recording
};

struct SwapchainData {
    DeviceData*    device    = nullptr;
    VkSwapchainKHR handle    = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR appInfo{}; // what the app asked for (pNext not kept)
    VkFormat       appFormat = VK_FORMAT_UNDEFINED;
    VkFormat       outFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR outColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    OutputMode     mode      = OutputMode::ScRGB;
    VkExtent2D     extent{};
    std::vector<uint32_t> queueFamilies;  // copied from appInfo (CONCURRENT sharing)
    bool           appMutable = false;     // app asked for MUTABLE_FORMAT
    std::vector<VkFormat> appViewFormats;  // app's VkImageFormatListCreateInfo, if any
    bool           imagesReady = false;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<ImageSlot> slots;
};

// ---------------------------------------------------------------------------
// Global layer state. One lock: the hot path (present) is once per frame.

struct Globals {
    std::mutex lock;
    std::unordered_map<void*, std::unique_ptr<InstanceData>>  instances; // by dispatch key
    std::unordered_map<void*, std::unique_ptr<DeviceData>>    devices;   // by dispatch key
    std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> swapchains;
};
Globals& globals();

template <typename T>
inline void* dispatchKey(T handle) { return *reinterpret_cast<void**>(handle); }

InstanceData* findInstance(void* key);
DeviceData*   findDevice(void* key);

// swapchain.cpp
VkResult CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
void     DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VkResult GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
void     releaseSwapchain(SwapchainData& sc); // frees our per-image resources
void     destroyDeviceResources(DeviceData& dev);

// present.cpp
VkResult QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
bool     ensurePipelines(DeviceData& dev);
VkCommandPool commandPoolFor(DeviceData& dev, uint32_t family);
SubmitFenceRef acquireSubmitFence(DeviceData& dev);

// formats.cpp
bool        isSdrSourceFormat(VkFormat f);
VkFormat    unormViewFormat(VkFormat f);   // SRGB -> UNORM sibling, else same
bool        isSrgbFormat(VkFormat f);
const char* formatName(VkFormat f);
const char* colorSpaceName(VkColorSpaceKHR cs);

} // namespace hdrket
