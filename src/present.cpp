// SPDX-License-Identifier: MIT
//
// Present hook: one compute dispatch per presented image, on the presenting
// queue, chained between the app's semaphores and the real present. Frame
// pacing is never touched: no waits beyond the fence guarding a command
// buffer we are about to re-record, which has long completed by then.

#include "hdrket.hpp"
#include "log.hpp"
#include "params.hpp"

#include "itm_hdr10_spv.h"
#include "itm_scrgb_spv.h"

#include <cstdlib>
#include <cstring>

namespace hdrket {

static float envFloat(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (!v || !*v)
        return fallback;
    char* end = nullptr;
    float f   = std::strtof(v, &end);
    return end != v ? f : fallback;
}

ItmParams currentParams()
{
    static const ItmParams params = [] {
        ItmParams p;
        p.peakNits       = envFloat("HDRKET_PEAK_NITS", p.peakNits);
        p.paperWhiteNits = envFloat("HDRKET_PAPER_WHITE", p.paperWhiteNits);
        p.expansion      = envFloat("HDRKET_EXPANSION", p.expansion);
        p.saturation     = envFloat("HDRKET_SATURATION", p.saturation);
        p.blackFloor     = envFloat("HDRKET_BLACK_FLOOR", p.blackFloor);
        p.splitX         = envFloat("HDRKET_SPLIT", p.splitX);
        const char* eotf = std::getenv("HDRKET_EOTF");
        if (eotf && !std::strcmp(eotf, "srgb"))
            p.flags |= ITM_SRGB_DECODE;
        if (envFloat("HDRKET_BYPASS", 0.0f) != 0.0f)
            p.flags |= ITM_BYPASS;
        HK_INFO("params: peak %.0f nits, paper white %.0f nits, expansion %.2f, saturation %.2f, black floor %.3f, %s",
                p.peakNits, p.paperWhiteNits, p.expansion, p.saturation, p.blackFloor,
                (p.flags & ITM_SRGB_DECODE) ? "sRGB decode" : "gamma 2.2 decode");
        return p;
    }();
    return params;
}

// ---------------------------------------------------------------------------
// Device-wide objects

static VkPipeline createPipeline(DeviceData& dev, const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = size;
    smci.pCode    = code;
    VkShaderModule module;
    if (dev.vk.CreateShaderModule(dev.device, &smci, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName  = "main";
    cpci.layout       = dev.pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    dev.vk.CreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    dev.vk.DestroyShaderModule(dev.device, module, nullptr);
    return pipeline;
}

bool ensurePipelines(DeviceData& dev)
{
    if (dev.pipelineScRGB && dev.pipelineHDR10)
        return true;
    auto& vk = dev.vk;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod       = 0.0f;
    if (!dev.sampler && vk.CreateSampler(dev.device, &sci, nullptr, &dev.sampler) != VK_SUCCESS)
        return false;

    if (!dev.setLayout) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding            = 0;
        bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount    = 1;
        bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[0].pImmutableSamplers = &dev.sampler;
        bindings[1].binding            = 1;
        bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount    = 1;
        bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 2;
        dslci.pBindings    = bindings;
        if (vk.CreateDescriptorSetLayout(dev.device, &dslci, nullptr, &dev.setLayout) != VK_SUCCESS)
            return false;
    }

    if (!dev.pipelineLayout) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ItmParams)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &dev.setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &range;
        if (vk.CreatePipelineLayout(dev.device, &plci, nullptr, &dev.pipelineLayout) != VK_SUCCESS)
            return false;
    }

    if (!dev.pipelineScRGB)
        dev.pipelineScRGB = createPipeline(dev, itm_scrgb_spv, sizeof(itm_scrgb_spv));
    if (!dev.pipelineHDR10)
        dev.pipelineHDR10 = createPipeline(dev, itm_hdr10_spv, sizeof(itm_hdr10_spv));
    if (!dev.pipelineScRGB || !dev.pipelineHDR10) {
        HK_ERR("compute pipeline creation failed");
        return false;
    }
    return true;
}

VkCommandPool commandPoolFor(DeviceData& dev, uint32_t family)
{
    auto it = dev.commandPools.find(family);
    if (it != dev.commandPools.end())
        return it->second;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    VkCommandPool pool    = VK_NULL_HANDLE;
    dev.vk.CreateCommandPool(dev.device, &cpci, nullptr, &pool);
    dev.commandPools[family] = pool;
    return pool;
}

SubmitFenceRef acquireSubmitFence(DeviceData& dev)
{
    VkFence fence = VK_NULL_HANDLE;
    if (!dev.freeFences.empty()) {
        fence = dev.freeFences.back();
        dev.freeFences.pop_back();
        // Free fences are signalled: every slot waits before letting go, and
        // a fence is only reset right before it is submitted.
        dev.vk.WaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
    } else {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (dev.vk.CreateFence(dev.device, &fci, nullptr, &fence) != VK_SUCCESS)
            return nullptr;
    }
    DeviceData* owner = &dev;
    return SubmitFenceRef(new SubmitFence{owner, fence}, [](SubmitFence* f) {
        f->device->freeFences.push_back(f->fence);
        delete f;
    });
}

void destroyDeviceResources(DeviceData& dev)
{
    auto& vk = dev.vk;
    vk.DeviceWaitIdle(dev.device);
    for (VkFence f : dev.freeFences)
        vk.DestroyFence(dev.device, f, nullptr);
    dev.freeFences.clear();
    for (auto& [family, pool] : dev.commandPools)
        vk.DestroyCommandPool(dev.device, pool, nullptr);
    dev.commandPools.clear();
    if (dev.pipelineScRGB)
        vk.DestroyPipeline(dev.device, dev.pipelineScRGB, nullptr);
    if (dev.pipelineHDR10)
        vk.DestroyPipeline(dev.device, dev.pipelineHDR10, nullptr);
    if (dev.pipelineLayout)
        vk.DestroyPipelineLayout(dev.device, dev.pipelineLayout, nullptr);
    if (dev.setLayout)
        vk.DestroyDescriptorSetLayout(dev.device, dev.setLayout, nullptr);
    if (dev.sampler)
        vk.DestroySampler(dev.device, dev.sampler, nullptr);
}

// ---------------------------------------------------------------------------
// Per-frame work

static void imageBarrier(VkImageMemoryBarrier& b, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess)
{
    b                     = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = srcAccess;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

static bool record(DeviceData& dev, SwapchainData& sc, ImageSlot& s, const ItmParams& params)
{
    auto& vk = dev.vk;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.BeginCommandBuffer(s.cmd, &begin) != VK_SUCCESS)
        return false;

    // The app left its image in PRESENT_SRC; the semaphore wait (at the
    // compute stage) makes its writes visible. The real image's previous
    // contents are irrelevant.
    VkImageMemoryBarrier pre[2];
    imageBarrier(pre[0], s.shadow, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                 VK_ACCESS_SHADER_READ_BIT);
    imageBarrier(pre[1], s.real, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
    vk.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, pre);

    VkPipeline pipeline = sc.mode == OutputMode::HDR10 ? dev.pipelineHDR10 : dev.pipelineScRGB;
    vk.CmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vk.CmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev.pipelineLayout, 0, 1, &s.set, 0, nullptr);
    ItmParams pc = params;
    pc.width     = sc.extent.width;
    pc.height    = sc.extent.height;
    vk.CmdPushConstants(s.cmd, dev.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vk.CmdDispatch(s.cmd, (sc.extent.width + 7) / 8, (sc.extent.height + 7) / 8, 1);

    // Hand both images back in the layouts their owners expect.
    VkImageMemoryBarrier post[2];
    imageBarrier(post[0], s.real, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_SHADER_WRITE_BIT, 0);
    imageBarrier(post[1], s.shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
    vk.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, post);

    return vk.EndCommandBuffer(s.cmd) == VK_SUCCESS;
}

VkResult QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    DeviceData* dev = nullptr;
    std::vector<VkSemaphore> ourSemaphores;
    VkPresentInfoKHR info = *pPresentInfo;
    {
        std::lock_guard l(globals().lock);
        dev = findDevice(dispatchKey(queue));
        if (!dev)
            return VK_ERROR_DEVICE_LOST;

        std::vector<std::pair<SwapchainData*, uint32_t>> active;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            auto it = globals().swapchains.find(pPresentInfo->pSwapchains[i]);
            if (it != globals().swapchains.end() && it->second->imagesReady)
                active.emplace_back(it->second.get(), pPresentInfo->pImageIndices[i]);
        }

        if (!active.empty()) {
            auto fam = dev->queueFamily.find(queue);
            if (fam == dev->queueFamily.end() || !(dev->families[fam->second].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                static bool warned = false;
                if (!warned)
                    HK_ERR("present queue has no compute support; HDR output will be wrong");
                warned = true;
                active.clear();
            }

            SubmitFenceRef fence = active.empty() ? nullptr : acquireSubmitFence(*dev);
            if (!fence)
                active.clear();

            const ItmParams          params = currentParams();
            std::vector<VkCommandBuffer> cmds;
            for (auto [sc, index] : active) {
                ImageSlot& s = sc->slots[index];
                if (s.inFlight) {
                    dev->vk.WaitForFences(dev->device, 1, &s.inFlight->fence, VK_TRUE, UINT64_MAX);
                    s.inFlight.reset();
                }
                if (s.cmd && s.cmdFamily != fam->second) {
                    dev->vk.FreeCommandBuffers(dev->device, commandPoolFor(*dev, s.cmdFamily), 1, &s.cmd);
                    s.cmd = VK_NULL_HANDLE;
                }
                if (!s.cmd) {
                    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    cbai.commandPool        = commandPoolFor(*dev, fam->second);
                    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cbai.commandBufferCount = 1;
                    if (dev->vk.AllocateCommandBuffers(dev->device, &cbai, &s.cmd) != VK_SUCCESS)
                        continue;
                    dev->setLoaderData(dev->device, s.cmd);
                    s.cmdFamily = fam->second;
                }
                if (!record(*dev, *sc, s, params))
                    continue;
                cmds.push_back(s.cmd);
                ourSemaphores.push_back(s.done);
                s.inFlight = fence;
            }

            if (!cmds.empty()) {
                std::vector<VkPipelineStageFlags> stages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount   = pPresentInfo->waitSemaphoreCount;
                submit.pWaitSemaphores      = pPresentInfo->pWaitSemaphores;
                submit.pWaitDstStageMask    = stages.data();
                submit.commandBufferCount   = static_cast<uint32_t>(cmds.size());
                submit.pCommandBuffers      = cmds.data();
                submit.signalSemaphoreCount = static_cast<uint32_t>(ourSemaphores.size());
                submit.pSignalSemaphores    = ourSemaphores.data();
                dev->vk.ResetFences(dev->device, 1, &fence->fence);
                VkResult res = dev->vk.QueueSubmit(queue, 1, &submit, fence->fence);
                if (res != VK_SUCCESS) {
                    HK_ERR("QueueSubmit failed (%d)", res);
                    return res;
                }
                info.waitSemaphoreCount = static_cast<uint32_t>(ourSemaphores.size());
                info.pWaitSemaphores    = ourSemaphores.data();
            }
        }
    }
    // Present outside the lock: it may block on vblank.
    return dev->vk.QueuePresentKHR(queue, &info);
}

} // namespace hdrket
