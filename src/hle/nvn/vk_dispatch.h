// Vulkan entry points loaded at run time (VK_NO_PROTOTYPES): the driver can then be the system's libvulkan or a
// custom one loaded through libadrenotools (e.g. Mesa Turnip), see LoadVulkanLibrary in vk_backend.cpp.
// Each function is a global pointer named like the Vulkan function, so call sites are unchanged.
#pragma once

// Before an instance exists (vkGetInstanceProcAddr(NULL, ...)).
#define DSR_VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance)

// Instance and physical-device level (vkGetInstanceProcAddr(instance, ...)).
#define DSR_VK_INSTANCE_FUNCS(X)                  \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceFeatures2)               \
    X(vkGetPhysicalDeviceFormatProperties)        \
    X(vkGetPhysicalDeviceImageFormatProperties)   \
    X(vkGetPhysicalDeviceMemoryProperties)        \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)       \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)  \
    X(vkEnumerateDeviceExtensionProperties)       \
    X(vkCreateDevice)                             \
    X(vkGetDeviceProcAddr)                        \
    X(vkCreateAndroidSurfaceKHR)                  \
    X(vkDestroySurfaceKHR)

// Device level (vkGetDeviceProcAddr once the device exists; the instance-level lookup before that).
#define DSR_VK_DEVICE_FUNCS(X)       \
    X(vkAcquireNextImageKHR)         \
    X(vkAllocateCommandBuffers)      \
    X(vkAllocateDescriptorSets)      \
    X(vkAllocateMemory)              \
    X(vkBeginCommandBuffer)          \
    X(vkBindBufferMemory)            \
    X(vkBindImageMemory)             \
    X(vkCmdBeginQuery)               \
    X(vkCmdBeginRenderPass)          \
    X(vkCmdBindDescriptorSets)       \
    X(vkCmdBindIndexBuffer)          \
    X(vkCmdBindPipeline)             \
    X(vkCmdBindVertexBuffers)        \
    X(vkCmdBlitImage)                \
    X(vkCmdClearAttachments)         \
    X(vkCmdClearColorImage)          \
    X(vkCmdClearDepthStencilImage)   \
    X(vkCmdCopyBufferToImage)        \
    X(vkCmdCopyImage)                \
    X(vkCmdCopyImageToBuffer)        \
    X(vkCmdDraw)                     \
    X(vkCmdDrawIndexed)              \
    X(vkCmdEndQuery)                 \
    X(vkCmdEndRenderPass)            \
    X(vkCmdPipelineBarrier)          \
    X(vkCmdPushConstants)            \
    X(vkCmdResetQueryPool)           \
    X(vkCmdSetBlendConstants)        \
    X(vkCmdSetScissor)               \
    X(vkCmdSetStencilCompareMask)    \
    X(vkCmdSetDepthBias)             \
    X(vkCmdSetStencilReference)      \
    X(vkCmdSetStencilWriteMask)      \
    X(vkCmdSetViewport)              \
    X(vkCmdWriteTimestamp)           \
    X(vkCreateBuffer)                \
    X(vkCreateCommandPool)           \
    X(vkCreateDescriptorPool)        \
    X(vkCreateDescriptorSetLayout)   \
    X(vkCreateFence)                 \
    X(vkCreateFramebuffer)           \
    X(vkCreateGraphicsPipelines)     \
    X(vkCreateImage)                 \
    X(vkCreateImageView)             \
    X(vkCreatePipelineCache)         \
    X(vkCreatePipelineLayout)        \
    X(vkCreateQueryPool)             \
    X(vkCreateRenderPass)            \
    X(vkCreateSampler)               \
    X(vkCreateSemaphore)             \
    X(vkCreateShaderModule)          \
    X(vkCreateSwapchainKHR)          \
    X(vkDestroyBuffer)               \
    X(vkDestroyFramebuffer)          \
    X(vkDestroyImage)                \
    X(vkDestroyImageView)            \
    X(vkDestroySwapchainKHR)         \
    X(vkDeviceWaitIdle)              \
    X(vkEndCommandBuffer)            \
    X(vkFreeMemory)                  \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceQueue)              \
    X(vkGetFenceStatus)              \
    X(vkGetImageMemoryRequirements)  \
    X(vkGetPipelineCacheData)        \
    X(vkGetQueryPoolResults)         \
    X(vkGetSwapchainImagesKHR)       \
    X(vkMapMemory)                   \
    X(vkQueuePresentKHR)             \
    X(vkQueueSubmit)                 \
    X(vkQueueWaitIdle)               \
    X(vkResetCommandBuffer)          \
    X(vkResetDescriptorPool)         \
    X(vkResetFences)                 \
    X(vkUnmapMemory)                 \
    X(vkUpdateDescriptorSets)        \
    X(vkWaitForFences)

#define DSR_VK_DECLARE(name) inline PFN_##name name = nullptr;
inline PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
DSR_VK_GLOBAL_FUNCS(DSR_VK_DECLARE)
DSR_VK_INSTANCE_FUNCS(DSR_VK_DECLARE)
DSR_VK_DEVICE_FUNCS(DSR_VK_DECLARE)
#undef DSR_VK_DECLARE
