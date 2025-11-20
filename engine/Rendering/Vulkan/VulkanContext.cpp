#include "VulkanContext.h"
#include <VkBootstrap.h>
#include <iostream>
#include <set>
#include <algorithm>

VulkanContext::~VulkanContext() {
    cleanup();
}

bool VulkanContext::init(GLFWwindow* window, bool enableValidation) {
    m_window = window;
    m_enableValidation = enableValidation;
    
    if (!createInstance(enableValidation)) return false;
    if (enableValidation && !setupDebugMessenger()) return false;
    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createVMAAllocator()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createDepthResources()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    if (!createPipelineCache()) return false;
    
    // Populate public members for ImGui and other systems
    instance = m_instance;
    physicalDevice = m_physicalDevice;
    device = m_device;
    allocator = m_allocator;
    graphicsQueue = m_graphicsQueue;
    renderPass = m_renderPass;
    swapchainImages = m_swapchainImages;
    
    // Create descriptor pool for ImGui
    VkDescriptorPoolSize pool_sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * 11;
    pool_info.poolSizeCount = 11;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &descriptorPool) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create descriptor pool\n";
        return false;
    }
    
    std::cout << "[Vulkan] Initialization complete\n";
    return true;
}

bool VulkanContext::createInstance(bool enableValidation) {
    vkb::InstanceBuilder builder;
    
    builder.set_app_name("Aetherial MMORPG")
           .set_engine_name("Aetherial Engine")
           .require_api_version(1, 3, 0);
    
    if (enableValidation) {
        builder.enable_validation_layers()
               .use_default_debug_messenger();
    }
    
    auto instRet = builder.build();
    if (!instRet) {
        std::cerr << "[Vulkan] Failed to create instance: " << instRet.error().message() << "\n";
        return false;
    }
    
    m_instance = instRet.value().instance;
    m_debugMessenger = instRet.value().debug_messenger;
    
    std::cout << "[Vulkan] Instance created\n";
    return true;
}

bool VulkanContext::setupDebugMessenger() {
    // Already set up by vk-bootstrap
    return true;
}

bool VulkanContext::createSurface(GLFWwindow* window) {
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create window surface\n";
        return false;
    }
    
    std::cout << "[Vulkan] Surface created\n";
    return true;
}

bool VulkanContext::pickPhysicalDevice() {
    // Get the vkb::Instance wrapper first
    vkb::InstanceBuilder builder;
    auto instRet = builder.set_app_name("Aetherial").build();
    if (!instRet) {
        std::cerr << "[Vulkan] Failed to recreate instance wrapper\n";
        return false;
    }
    vkb::Instance vkbInstance = instRet.value();
    vkbInstance.instance = m_instance; // Use our already-created instance
    
    vkb::PhysicalDeviceSelector selector{vkbInstance};
    
    auto physRet = selector
        .set_surface(m_surface)
        .set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)  // FORCE discrete GPU (RTX 3060)
        .require_dedicated_transfer_queue()
        .select();
    
    if (!physRet) {
        std::cerr << "[Vulkan] Failed to select physical device: " << physRet.error().message() << "\n";
        return false;
    }
    
    vkb::PhysicalDevice vkbPhysicalDevice = physRet.value();
    m_physicalDevice = vkbPhysicalDevice.physical_device;
    
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    std::cout << "[Vulkan] Selected GPU: " << properties.deviceName 
              << " (Type: " << (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "DISCRETE" : 
                               properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "INTEGRATED" : "OTHER")
              << ")\n";
    
    return true;
}

bool VulkanContext::createLogicalDevice() {
    // Re-select physical device to get proper vkb::PhysicalDevice wrapper
    vkb::InstanceBuilder instBuilder;
    auto instRet = instBuilder.set_app_name("Aetherial").build();
    vkb::Instance vkbInstance = instRet.value();
    vkbInstance.instance = m_instance;
    
    vkb::PhysicalDeviceSelector selector{vkbInstance};
    auto physRet = selector
        .set_surface(m_surface)
        .set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)  // Match pickPhysicalDevice()
        .select();
    
    if (!physRet) {
        std::cerr << "[Vulkan] Failed to get physical device wrapper\n";
        return false;
    }
    
    vkb::PhysicalDevice vkbPhysicalDevice = physRet.value();
    
    // Enable Vulkan 1.1 features for shader draw parameters
    VkPhysicalDeviceVulkan11Features vulkan11Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    vulkan11Features.shaderDrawParameters = VK_TRUE;
    
    // Query device features to enable multi-draw indirect
    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(vkbPhysicalDevice.physical_device, &supportedFeatures);
    
    VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    deviceFeatures2.features.multiDrawIndirect = VK_TRUE;
    deviceFeatures2.features.drawIndirectFirstInstance = VK_TRUE;
    deviceFeatures2.pNext = &vulkan11Features;
    
    vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};
    deviceBuilder.add_pNext(&deviceFeatures2);
    
    auto devRet = deviceBuilder.build();
    if (!devRet) {
        std::cerr << "[Vulkan] Failed to create logical device: " << devRet.error().message() << "\n";
        return false;
    }
    
    vkb::Device vkbDevice = devRet.value();
    m_device = vkbDevice.device;
    
    // Log enabled features that might affect memory requirements
    std::cout << "[Vulkan] Enabled device features:\n";
    std::cout << "  multiDrawIndirect: " << (deviceFeatures2.features.multiDrawIndirect ? "YES" : "NO") << "\n";
    std::cout << "  drawIndirectFirstInstance: " << (deviceFeatures2.features.drawIndirectFirstInstance ? "YES" : "NO") << "\n";
    std::cout << "  shaderDrawParameters (VK1.1): " << (vulkan11Features.shaderDrawParameters ? "YES" : "NO") << "\n";
    
    // Get queues
    auto graphicsQueueRet = vkbDevice.get_queue(vkb::QueueType::graphics);
    if (!graphicsQueueRet) {
        std::cerr << "[Vulkan] Failed to get graphics queue\n";
        return false;
    }
    m_graphicsQueue = graphicsQueueRet.value();
    
    // Get queue family index for ImGui
    auto graphicsQueueFamilyRet = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueueFamilyRet) {
        std::cerr << "[Vulkan] Failed to get graphics queue family\n";
        return false;
    }
    graphicsQueueFamily = graphicsQueueFamilyRet.value();
    
    auto presentQueueRet = vkbDevice.get_queue(vkb::QueueType::present);
    if (!presentQueueRet) {
        std::cerr << "[Vulkan] Failed to get present queue\n";
        return false;
    }
    m_presentQueue = presentQueueRet.value();
    
    std::cout << "[Vulkan] Logical device created\n";
    
    // Populate public members for easy access
    instance = m_instance;
    physicalDevice = m_physicalDevice;
    device = m_device;
    
    return true;
}

bool VulkanContext::createVMAAllocator() {
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;
    
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;
    
    VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    if (result != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create VMA allocator\n";
        return false;
    }
    
    allocator = m_allocator;  // Populate public member
    
    // Debug: Print available memory types
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    std::cout << "[Vulkan] GPU Memory Types (" << memProps.memoryTypeCount << " total):\n";
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
        std::cout << "  Type " << i << ": Heap " << memProps.memoryTypes[i].heapIndex;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) std::cout << " DEVICE_LOCAL";
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) std::cout << " HOST_VISIBLE";
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) std::cout << " HOST_COHERENT";
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) std::cout << " HOST_CACHED";
        std::cout << "\n";
    }
    
    std::cout << "[Vulkan] VMA allocator created\n";
    return true;
}

bool VulkanContext::createSwapchain() {
    vkb::SwapchainBuilder swapchainBuilder{m_physicalDevice, m_device, m_surface};
    
    int width, height;
    glfwGetFramebufferSize(m_window, &width, &height);
    
    auto swapRet = swapchainBuilder
        .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR) // Guaranteed to be available
        .set_desired_extent(width, height)
        .build();
    
    if (!swapRet) {
        std::cerr << "[Vulkan] Failed to create swapchain: " << swapRet.error().message() << "\n";
        return false;
    }
    
    vkb::Swapchain vkbSwapchain = swapRet.value();
    m_swapchain = vkbSwapchain.swapchain;
    m_swapchainImages = vkbSwapchain.get_images().value();
    m_swapchainImageViews = vkbSwapchain.get_image_views().value();
    m_swapchainImageFormat = vkbSwapchain.image_format;
    m_swapchainExtent = vkbSwapchain.extent;
    
    std::cout << "[Vulkan] Swapchain created: " << m_swapchainExtent.width << "x" << m_swapchainExtent.height << "\n";
    return true;
}

bool VulkanContext::createImageViews() {
    // Already created by vk-bootstrap
    return true;
}

bool VulkanContext::createRenderPass() {
    // Color attachment
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    // Depth attachment
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;
    
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    
    VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    
    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create render pass\n";
        return false;
    }
    
    std::cout << "[Vulkan] Render pass created\n";
    return true;
}

bool VulkanContext::createFramebuffers() {
    m_swapchainFramebuffers.resize(m_swapchainImageViews.size());
    
    for (size_t i = 0; i < m_swapchainImageViews.size(); i++) {
        VkImageView attachments[] = {
            m_swapchainImageViews[i],
            m_depthImageView
        };
        
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = m_swapchainExtent.width;
        framebufferInfo.height = m_swapchainExtent.height;
        framebufferInfo.layers = 1;
        
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS) {
            std::cerr << "[Vulkan] Failed to create framebuffer\n";
            return false;
        }
    }
    
    std::cout << "[Vulkan] Framebuffers created\n";
    return true;
}

bool VulkanContext::createCommandPool() {
    VulkanQueueFamilyIndices queueFamilyIndices = findQueueFamilies(m_physicalDevice);
    
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
    
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create command pool\n";
        return false;
    }
    
    std::cout << "[Vulkan] Command pool created\n";
    return true;
}

bool VulkanContext::createCommandBuffers() {
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    
    if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to allocate command buffers\n";
        return false;
    }
    
    std::cout << "[Vulkan] Command buffers allocated\n";
    return true;
}

bool VulkanContext::createSyncObjects() {
    m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            std::cerr << "[Vulkan] Failed to create synchronization objects\n";
            return false;
        }
    }
    
    std::cout << "[Vulkan] Synchronization objects created\n";
    return true;
}

bool VulkanContext::beginFrame(uint32_t& imageIndex) {
    return beginFrame(imageIndex, true);
}

bool VulkanContext::beginFrame(uint32_t& imageIndex, bool startRenderPass) {
    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);
    
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                           m_imageAvailableSemaphores[m_currentFrame],
                                           VK_NULL_HANDLE, &imageIndex);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::cerr << "[Vulkan] Failed to acquire swapchain image, result=" << result << "\n";
        return false;
    }
    
    // Only reset fence if we're actually going to submit work
    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
    
    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    
    if (vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to begin recording command buffer\n";
        return false;
    }
    
    if (startRenderPass) {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_swapchainExtent;
        
        VkClearValue clearValues[2];
        clearValues[0].color = {{0.53f, 0.81f, 0.92f, 1.0f}};  // Sky blue
        clearValues[1].depthStencil = {1.0f, 0};
        
        renderPassInfo.clearValueCount = 2;
        renderPassInfo.pClearValues = clearValues;
        
        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    
    return true;
}

void VulkanContext::beginRenderPass(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchainExtent;
    
    VkClearValue clearValues[2];
    clearValues[0].color = {{0.53f, 0.81f, 0.92f, 1.0f}};  // Sky blue
    clearValues[1].depthStencil = {1.0f, 0};
    
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;
    
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanContext::endFrame(uint32_t imageIndex) {
    endFrame(imageIndex, true);
}

void VulkanContext::endFrame(uint32_t imageIndex, bool endRenderPass) {
    if (endRenderPass) {
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
    }
    
    if (vkEndCommandBuffer(m_commandBuffers[m_currentFrame]) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to record command buffer\n";
        return;
    }
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphores[m_currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
    
    VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[m_currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    
    VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);
    if (submitResult != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to submit draw command buffer, result=" << submitResult << "\n";
        return;
    }
    
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    
    VkSwapchainKHR swapchains[] = {m_swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;
    
    VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else if (result != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to present swapchain image, result=" << result << "\n";
    }
    
    // Advance to next frame
    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    
    // Wait for present queue to finish to avoid semaphore reuse issues
    // This is not optimal but ensures correctness for Phase 1
    vkQueueWaitIdle(m_presentQueue);
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

VulkanQueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) {
    VulkanQueueFamilyIndices indices;
    
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
    
    int i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }
        
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        
        if (presentSupport) {
            indices.presentFamily = i;
        }
        
        if (indices.isComplete()) {
            break;
        }
        
        i++;
    }
    
    return indices;
}

VulkanSwapChainSupportDetails VulkanContext::querySwapChainSupport(VkPhysicalDevice device) {
    VulkanSwapChainSupportDetails details;
    
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities);
    
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, details.formats.data());
    }
    
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr);
    
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, details.presentModes.data());
    }
    
    return details;
}

VkSurfaceFormatKHR VulkanContext::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    
    return availableFormats[0];
}

VkPresentModeKHR VulkanContext::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanContext::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    } else {
        int width, height;
        glfwGetFramebufferSize(m_window, &width, &height);
        
        VkExtent2D actualExtent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };
        
        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        
        return actualExtent;
    }
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) {
    VulkanQueueFamilyIndices indices = findQueueFamilies(device);
    bool extensionsSupported = checkDeviceExtensionSupport(device);
    
    bool swapChainAdequate = false;
    if (extensionsSupported) {
        VulkanSwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }
    
    return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

bool VulkanContext::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());
    
    std::set<std::string> requiredExtensions(m_deviceExtensions.begin(), m_deviceExtensions.end());
    
    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    
    return requiredExtensions.empty();
}

void VulkanContext::recreateSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }
    
    vkDeviceWaitIdle(m_device);
    
    cleanupSwapchain();
    
    createSwapchain();
    createImageViews();
    createDepthResources();
    createFramebuffers();
}

VkFormat VulkanContext::findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
        
        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
            return format;
        } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    
    std::cerr << "[Vulkan] Failed to find supported format\n";
    return candidates[0];
}

VkFormat VulkanContext::findDepthFormat() {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
    );
}

bool VulkanContext::createDepthResources() {
    m_depthFormat = findDepthFormat();
    
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_swapchainExtent.width;
    imageInfo.extent.height = m_swapchainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    
    if (vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_depthImage, &m_depthImageAllocation, nullptr) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create depth image\n";
        return false;
    }
    
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create depth image view\n";
        return false;
    }
    
    std::cout << "[Vulkan] Depth resources created\n";
    return true;
}

void VulkanContext::cleanupSwapchain() {
    // Destroy depth resources
    if (m_depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_depthImageView, nullptr);
        m_depthImageView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_depthImage, m_depthImageAllocation);
        m_depthImage = VK_NULL_HANDLE;
        m_depthImageAllocation = VK_NULL_HANDLE;
    }
    
    for (auto framebuffer : m_swapchainFramebuffers) {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    
    for (auto imageView : m_swapchainImageViews) {
        vkDestroyImageView(m_device, imageView, nullptr);
    }
    
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
}

void VulkanContext::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(m_device);
    
    cleanupSwapchain();
    
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }
    
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
        allocator = VK_NULL_HANDLE;
    }
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
    }
    
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    
    if (m_pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
        pipelineCache = VK_NULL_HANDLE;
    }
    
    vkDestroyDevice(m_device, nullptr);
    
    if (m_enableValidation && m_debugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(m_instance, m_debugMessenger, nullptr);
        }
    }
    
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    
    std::cout << "[Vulkan] Cleanup complete\n";
}

bool VulkanContext::createPipelineCache() {
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    
    if (vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        std::cerr << "[Vulkan] Failed to create pipeline cache\n";
        return false;
    }
    
    pipelineCache = m_pipelineCache;
    std::cout << "[Vulkan] Pipeline cache created\n";
    return true;
}
