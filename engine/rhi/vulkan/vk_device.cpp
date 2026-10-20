#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "rhi/vulkan/vk_device.h"
#include "core/log.h"
#include "platform/window.h"

#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>

#include <cstdlib>
#include <cstring>

namespace keel::vk {

void check(VkResult r, const char* expr, const char* file, int line) {
    if (r == VK_SUCCESS) return;
    log::error("{} failed with VkResult {} ({}:{})", expr, int(r), file, line);
    std::abort();
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Coarse but correct for now; per-pass barriers come with the render graph.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) log::error("[vulkan] {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) log::warn("[vulkan] {}", data->pMessage);
    return VK_FALSE;
}

bool Device::init(const Window& window, bool validation) {
    window_ = &window;
#if defined(__APPLE__)
    // The library SDL loaded for the window (the app's MoltenVK, or a Vulkan SDK's loader).
    if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr())) volkInitializeCustom(gipa);
    else
#endif
    if (volkInitialize() != VK_SUCCESS) {
        log::error("No Vulkan loader found - is a Vulkan driver installed?");
        return false;
    }

    Uint32 extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name("Fry3DX").require_api_version(1, 3, 0).enable_extensions(extCount, sdlExts);
    if (validation) ib.request_validation_layers().set_debug_callback(debugCallback);
    auto inst = ib.build();
    if (!inst) {
        log::error("Vulkan instance: {}", inst.error().message());
        return false;
    }
    instance_ = inst->instance;
    messenger_ = inst->debug_messenger;
    volkLoadInstance(instance_);

    if (!SDL_Vulkan_CreateSurface(window.handle(), instance_, nullptr, &surface_)) {
        log::error("SDL_Vulkan_CreateSurface: {}", SDL_GetError());
        return false;
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // `discard` compiles to demote in Vulkan 1.3 SPIR-V
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures f10{};
    f10.samplerAnisotropy = VK_TRUE;
    f10.independentBlend = VK_TRUE; // (the G-buffer pass blends its attachments differently)
    auto phys = vkb::PhysicalDeviceSelector(*inst)
                    .set_surface(surface_)
                    .set_minimum_version(1, 3)
                    .set_required_features_13(f13)
                    .set_required_features_12(f12)
                    .set_required_features(f10)
                    .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                    .select();
    if (!phys) {
        log::error("No suitable GPU (needs Vulkan 1.3): {}", phys.error().message());
        return false;
    }
    vkb::PhysicalDevice pd = phys.value();
    physical_ = pd.physical_device;
    log::info("GPU: {}", pd.properties.deviceName);
    // Ray queries where the GPU has them (not needed: what uses them is skipped without).
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    asf.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rqf.rayQuery = VK_TRUE;
    const char* rt[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    rayQuery_ = pd.enable_extensions_if_present(3, rt) && pd.enable_extension_features_if_present(asf) && pd.enable_extension_features_if_present(rqf);
    log::info("Ray queries: {}", rayQuery_ ? "yes" : "no");

    memoryBudget_ = pd.enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME); // (real VRAM use, for the debug overlay)
    gpuName_ = pd.properties.deviceName;
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd.physical_device, &props2);
    gpuDriver_ = std::string(drv.driverName) + (drv.driverInfo[0] ? std::string(" ") + drv.driverInfo : std::string());

    auto dev = vkb::DeviceBuilder(pd).build();
    if (!dev) {
        log::error("Vulkan device: {}", dev.error().message());
        return false;
    }
    device_ = dev->device;
    volkLoadDevice(device_);
    queue_ = dev->get_queue(vkb::QueueType::graphics).value();
    queueFamily_ = dev->get_queue_index(vkb::QueueType::graphics).value();

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | (memoryBudget_ ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.instance = instance_;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.pVulkanFunctions = &fns;
    VK_CHECK(vmaCreateAllocator(&aci, &allocator_));

    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.acquired));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.inFlight));
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &immPool_));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = immPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &immCmd_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device_, &fci, nullptr, &immFence_));
    return createSwapchain();
}

void Device::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VK_CHECK(vkResetCommandBuffer(immCmd_, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(immCmd_, &bi));
    record(immCmd_);
    VK_CHECK(vkEndCommandBuffer(immCmd_));
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = immCmd_;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, immFence_));
    VK_CHECK(vkWaitForFences(device_, 1, &immFence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device_, 1, &immFence_));
}

Buffer Device::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible)
        aci.flags = (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Buffer b;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bci, &aci, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device_, &dai);
    return b;
}

void Device::destroyBuffer(Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(allocator_, b.buffer, b.alloc);
    b = {};
}

Buffer Device::uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, size);
    Buffer b = createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, b.buffer, 1, &copy);
    });
    destroyBuffer(staging);
    return b;
}

bool Device::createSwapchain() {
    uint32_t w, h;
    window_->pixelSize(w, h);
    if (w == 0 || h == 0) return false;

    vkDeviceWaitIdle(device_);
    VkSwapchainKHR old = swapchain_;
    auto sc = vkb::SwapchainBuilder(physical_, device_, surface_)
#if defined(__ANDROID__) // (the compositor turns the picture with the phone: drawn as it's seen)
                  .set_pre_transform_flags(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
#endif
                  .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                  .set_desired_present_mode(vsync_ ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
                  .add_fallback_present_mode(vsync_ ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR)
                  .set_desired_extent(w, h)
                  .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                  .set_old_swapchain(old)
                  .build();
    if (!sc) {
        log::error("Swapchain: {}", sc.error().message());
        return false;
    }
    destroySwapchainViews();
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    swapchain_ = sc->swapchain;
    swapchainFormat_ = sc->image_format;
    extent_ = sc->extent;
    images_ = sc->get_images().value();
    views_ = sc->get_image_views().value();
    renderDone_.resize(images_.size());
    for (VkSemaphore& s : renderDone_) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &s));
    }
    swapchainDirty_ = false;
    return true;
}

// A new surface on the window as it is now (Android makes a new one each time the app comes back).
bool Device::recreateSurface() {
    vkDeviceWaitIdle(device_);
    destroySwapchainViews();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr), swapchain_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr), surface_ = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_->handle(), instance_, nullptr, &surface_)) {
        log::warn("SDL_Vulkan_CreateSurface (again): {}", SDL_GetError());
        surface_ = VK_NULL_HANDLE;
        return false; // (tried again next frame)
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface_, &present);
    if (!present) log::warn("The new surface can't be presented to from this queue");
    surfaceLost_ = false;
    return createSwapchain();
}

void Device::destroySwapchainViews() {
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    views_.clear();
    renderDone_.clear();
}

VkCommandBuffer Device::beginFrame() {
    if (surfaceLost_ && !recreateSurface()) return nullptr;
    if (swapchainDirty_ && !createSwapchain()) return nullptr;

    Frame& f = frames_[frameIndex_];
    VK_CHECK(vkWaitForFences(device_, 1, &f.inFlight, VK_TRUE, UINT64_MAX));
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return nullptr;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        surfaceLost_ = true;
        return nullptr;
    }
    if (r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    VK_CHECK(vkResetFences(device_, 1, &f.inFlight));

    VK_CHECK(vkResetCommandBuffer(f.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    transitionImage(f.cmd, images_[imageIndex_], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return f.cmd;
}

void Device::endFrame() {
    Frame& f = frames_[frameIndex_];
    transitionImage(f.cmd, images_[imageIndex_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = renderDone_[imageIndex_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.inFlight));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
#if defined(__ANDROID__) // (suboptimal every frame while the phone's turned: the compositor turns it; only a new size matters)
    if (r == VK_SUBOPTIMAL_KHR) {
        uint32_t w = 0, h = 0;
        window_->pixelSize(w, h);
        if (w == extent_.width && h == extent_.height) r = VK_SUCCESS;
    }
#endif
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
    else if (r == VK_ERROR_SURFACE_LOST_KHR) surfaceLost_ = true;
    else VK_CHECK(r);

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Device::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (Frame& f : frames_) {
        vkDestroyCommandPool(device_, f.pool, nullptr);
        vkDestroySemaphore(device_, f.acquired, nullptr);
        vkDestroyFence(device_, f.inFlight, nullptr);
    }
    vkDestroyCommandPool(device_, immPool_, nullptr);
    vkDestroyFence(device_, immFence_, nullptr);
    destroySwapchainViews();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
}

void Device::vram(uint64_t& total, uint64_t& used, uint64_t& ours) const {
    total = used = ours = 0;
    if (!allocator_) return;
    const VkPhysicalDeviceMemoryProperties* mem = nullptr;
    vmaGetMemoryProperties(allocator_, &mem);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(allocator_, budgets);
    for (uint32_t h = 0; h < mem->memoryHeapCount; ++h) {
        if (!(mem->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        total += mem->memoryHeaps[h].size;
        used += budgets[h].usage; // (the whole process's, with VK_EXT_memory_budget; VMA's own blocks without)
        ours += budgets[h].statistics.blockBytes;
    }
}

} // namespace keel::vk
