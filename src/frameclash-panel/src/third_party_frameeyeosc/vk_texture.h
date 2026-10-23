// Copied unmodified from frameeyeosc (https://github.com/sasaken1102r/frameeyeosc, commit c4294a9, panel/src/vk_texture.h).
// Copyright (c) 2026 konsti219; Copyright (c) 2026 sasaken1102r. MIT License: see LICENSE.frameeyeosc in this folder.
// Vulkan texture for the overlay (passed via SetOverlayTexture).
// SetOverlayRaw leaves the texture missing for 15-40ms on every swap (flicker), and since it
// goes through shared memory it's also suspected of causing SIGBUS in the compositor on exit,
// so we don't use it.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

/**
 * A full set of Vulkan objects built for the GPU and extensions OpenVR requests.
 * Created after OpenVR init, destroyed after VR_Shutdown.
 */
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /**
     * Create the instance, device, queue, and command pool (OpenVR must already be initialized).
     * @param error reason for failure
     * @return true on success
     */
    bool init(std::string& error);

    /** Destroy everything created. Does nothing if nothing was created. */
    void destroy();

    /**
     * Find a memory type matching the given constraints.
     * @param typeBits bitmask of usable types (VkMemoryRequirements::memoryTypeBits)
     * @param properties required properties
     * @return the type index, or UINT32_MAX if none found
     */
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;

    /** @return true if initialized */
    bool ready() const { return device_ != VK_NULL_HANDLE; }

    /** @return the Vulkan instance */
    VkInstance instance() const { return instance_; }
    /** @return the GPU driving the HMD */
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    /** @return the logical device */
    VkDevice device() const { return device_; }
    /** @return the queue used for transfers */
    VkQueue queue() const { return queue_; }
    /** @return the queue family index */
    uint32_t queueFamily() const { return queueFamily_; }
    /** @return the command pool */
    VkCommandPool commandPool() const { return commandPool_; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
};

/**
 * A texture submitted to a single overlay. Keeps two images and writes to them alternately
 * (so we never overwrite an image while the compositor is still reading the previous one).
 */
class OverlayTexture {
public:
    OverlayTexture() = default;
    ~OverlayTexture();
    OverlayTexture(const OverlayTexture&) = delete;
    OverlayTexture& operator=(const OverlayTexture&) = delete;

    /**
     * Create the images, staging buffer, and command buffer.
     * @param context an initialized Vulkan context
     * @param width width in px
     * @param height height in px
     * @param error reason for failure
     * @return true on success
     */
    bool create(VulkanContext& context, int width, int height, std::string& error);

    /** Destroy everything created (after waiting for the GPU to finish). */
    void destroy();

    /**
     * Copy RGBA pixels into a GPU image and hand it to the overlay via SetOverlayTexture.
     * Waits for the transfer to finish before handing it off, so the image is complete by then.
     * @param overlayHandle vr::VROverlayHandle_t
     * @param rgba non-premultiplied 8-bit RGBA (width * height * 4 bytes)
     * @param error reason for failure
     * @return true on success
     */
    bool update(uint64_t overlayHandle, const uint8_t* rgba, std::string& error);

    /** @return true if already created */
    bool ready() const { return fence_ != VK_NULL_HANDLE; }

private:
    VulkanContext* context_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    VkImage images_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory imageMemory_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool imageUsed_[2] = {false, false};  ///< whether each image was ever written (decides the layout transition source)
    int next_ = 0;                        ///< the image to write next
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};
