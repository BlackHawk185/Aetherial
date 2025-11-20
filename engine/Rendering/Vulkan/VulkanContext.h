#pragma once

#include <vulkan/vulkan.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <GLFW/glfw3.h>
#include <vector>
#include <optional>

struct VulkanQueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    
    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct VulkanSwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    
    bool init(GLFWwindow* window, bool enableValidation = true);
    void cleanup();
    
    // Frame rendering
    bool beginFrame(uint32_t& imageIndex);
    bool beginFrame(uint32_t& imageIndex, bool startRenderPass); // Custom control
    void beginRenderPass(VkCommandBuffer cmd, uint32_t imageIndex); // Manual render pass start
    void endFrame(uint32_t imageIndex);
    void endFrame(uint32_t imageIndex, bool endRenderPass); // Custom control
    
    // One-time command buffer for transfers/updates (submit immediately)
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    
    // Getters
    VkInstance getInstance() const { return m_instance; }
    VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    VkDevice getDevice() const { return m_device; }
    VkQueue getGraphicsQueue() const { return m_graphicsQueue; }
    VkQueue getPresentQueue() const { return m_presentQueue; }
    VkSwapchainKHR getSwapchain() const { return m_swapchain; }
    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkCommandBuffer getCurrentCommandBuffer() const { return m_commandBuffers[m_currentFrame]; }
    VkCommandPool getCommandPool() const { return m_commandPool; }
    VkExtent2D getSwapchainExtent() const { return m_swapchainExtent; }
    VkFormat getSwapchainImageFormat() const { return m_swapchainImageFormat; }
    VkFormat getSwapchainFormat() const { return m_swapchainImageFormat; }
    VmaAllocator getAllocator() const { return m_allocator; }
    VkImageView getSwapchainImageView(uint32_t index) const { return m_swapchainImageViews[index]; }
    VkImageView getDepthImageView() const { return m_depthImageView; }
    VkImage getDepthImage() const { return m_depthImage; }
    VkFormat getDepthFormat() const { return m_depthFormat; }
    
    // Swapchain recreation
    void recreateSwapchain();
    
    // Public members for easy access (following Phase 1 pattern)
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    
    // Pipeline cache management
    bool createPipelineCache();
    
private:
    // Initialization helpers
    bool createInstance(bool enableValidation);
    bool setupDebugMessenger();
    bool createSurface(GLFWwindow* window);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVMAAllocator();
    
    // Helper functions
    VulkanQueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    VulkanSwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    bool isDeviceSuitable(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
    
    // Resource creation
    bool createDepthResources();
    
    // Cleanup helpers
    void cleanupSwapchain();
    
    // Vulkan handles
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    VkFormat m_swapchainImageFormat;
    VkExtent2D m_swapchainExtent;
    
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_swapchainFramebuffers;
    
    // Depth buffer
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VmaAllocation m_depthImageAllocation = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;
    
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    
    // Synchronization objects (double buffering)
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_currentFrame = 0;
    
    // Window handle (for swapchain recreation)
    GLFWwindow* m_window = nullptr;
    
    // Required device extensions
    const std::vector<const char*> m_deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    
    // Validation layers
    bool m_enableValidation = true;
    const std::vector<const char*> m_validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };
};
