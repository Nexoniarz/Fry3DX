#pragma once
// Vulkan 1.3 device, swapchain and frames in flight.
// Uses dynamic rendering and synchronization2; no render passes.
#include <volk.h>
#include <vk_mem_alloc.h>

#include <functional>
#include <string>
#include <vector>

namespace keel {
class Window;
}

namespace keel::vk {

#define VK_CHECK(x) ::keel::vk::check((x), #x, __FILE__, __LINE__)
void check(VkResult r, const char* expr, const char* file, int line);

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void* mapped = nullptr; // host-visible buffers only
};

class Device {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    bool init(const Window& window, bool validation);
    void shutdown();

    // Returns a command buffer recording into the current swapchain image (already in
    // COLOR_ATTACHMENT_OPTIMAL), or nullptr when no frame can be drawn (minimized, resizing).
    VkCommandBuffer beginFrame();
    void endFrame();

    // Records and runs commands right away, waiting for completion. For loading, not per frame.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
    // hostVisible: mapped CPU-writable memory (staging); otherwise device-local.
    // readback: host-cached memory the CPU reads from (screenshots); otherwise write-combined.
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback = false);
    void destroyBuffer(Buffer& b);
    // Device-local buffer filled with `data` through a staging copy.
    Buffer uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

    void setVsync(bool vsync) { vsync_ = vsync; swapchainDirty_ = true; }
    void markResized() { swapchainDirty_ = true; }
    void waitIdle() { vkDeviceWaitIdle(device_); }

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    // Hardware ray queries (VK_KHR_ray_query + acceleration structures): real-time GI uses them.
    bool rayQuery() const { return rayQuery_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VmaAllocator allocator() const { return allocator_; }
    const std::string& gpuName() const { return gpuName_; }
    const std::string& gpuDriver() const { return gpuDriver_; }
    // VRAM (the device-local heaps): total, used by everyone (ours if the driver can't say), ours.
    void vram(uint64_t& total, uint64_t& used, uint64_t& ours) const;

    VkFormat swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t frameIndex() const { return frameIndex_; } // which frame-in-flight slot is recording
    uint32_t imageCount() const { return uint32_t(images_.size()); }
    VkImage currentImage() const { return images_[imageIndex_]; }
    VkImageView currentImageView() const { return views_[imageIndex_]; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    bool createSwapchain();
    void destroySwapchainViews();

    const Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    bool rayQuery_ = false, memoryBudget_ = false;
    std::string gpuName_, gpuDriver_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> renderDone_; // per swapchain image, so present never reuses one in flight
    bool vsync_ = true;
    bool swapchainDirty_ = false;

    Frame frames_[kFramesInFlight];
    VkCommandPool immPool_ = VK_NULL_HANDLE;
    VkCommandBuffer immCmd_ = VK_NULL_HANDLE;
    VkFence immFence_ = VK_NULL_HANDLE;
    uint32_t frameIndex_ = 0;
    uint32_t imageIndex_ = 0;
};

} // namespace keel::vk
