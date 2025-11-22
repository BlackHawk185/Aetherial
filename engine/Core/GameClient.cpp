// GameClient.cpp - Client-side rendering and input implementation
#include "../RenderConfig.h"  // USE_VULKAN flag
#include "GameClient.h"

#include <GLFW/glfw3.h>

#ifdef USE_VULKAN
#include "../Rendering/Vulkan/VulkanContext.h"
#include "../Rendering/Vulkan/VulkanQuadRenderer.h"
#include "../Rendering/Vulkan/VulkanModelRenderer.h"
#include "../Rendering/Vulkan/VulkanDeferred.h"
#include "../Rendering/Vulkan/VulkanSkyRenderer.h"
#include "../Rendering/Vulkan/VulkanCloudRenderer.h"
#else
#include <glad/gl.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <iostream>
#include <memory>
#include <tuple>

#include "ClientWorld.h"
#include "../Profiling/Profiler.h"
#include "../World/BlockType.h"
#include "../ECS/ECS.h"
#include "../World/FluidComponents.h"  // For FluidParticleComponent (render-only on client)

#include "../Network/NetworkManager.h"
#include "../Network/NetworkMessages.h"
#include "../Rendering/Vulkan/VulkanBlockHighlighter.h"
#include "../UI/HUD.h"
#include "../UI/PeriodicTableUI.h"  // NEW: Periodic table UI for hotbar binding
#include "../Core/Window.h"
#include "../Rendering/TextureManager.h"
#include "../Physics/PhysicsSystem.h"  // For ground detection
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "../Time/TimeEffects.h"
#include "../Time/DayNightController.h"
#include "../World/VoxelChunk.h"  // For accessing voxel data

// External systems
extern TimeEffects* g_timeEffects;

// Global Vulkan renderer pointers (for chunk mesh uploads)
VulkanQuadRenderer* g_vulkanQuadRenderer = nullptr;
VulkanModelRenderer* g_vulkanModelRenderer = nullptr;

GameClient::GameClient()
{
    // Constructor
    m_networkManager = std::make_unique<NetworkManager>();  // Re-enabled with ENet

    // Initialize day/night controller
    m_dayNightController = std::make_unique<DayNightController>();
    g_dayNightController = m_dayNightController.get();
    
    // Initialize default hotbar elements (keys 1-9)
    m_hotbarElements = {
        Element::H,   // 1 - Hydrogen
        Element::C,   // 2 - Carbon
        Element::O,   // 3 - Oxygen
        Element::Si,  // 4 - Silicon
        Element::Na,  // 5 - Sodium
        Element::Cl,  // 6 - Chlorine
        Element::Ca,  // 7 - Calcium
        Element::Fe,  // 8 - Iron
        Element::Cu   // 9 - Copper
    };

    // Set up network callbacks
    if (auto client = m_networkManager->getClient())
    {
        client->onWorldStateReceived = [this](const WorldStateMessage& worldState)
        { this->handleWorldStateReceived(worldState); };

        client->onCompressedIslandReceived = [this](uint32_t islandID, const Vec3& position,
                                                    const uint8_t* voxelData, uint32_t dataSize)
        { this->handleCompressedIslandReceived(islandID, position, voxelData, dataSize); };

        client->onCompressedChunkReceived = [this](uint32_t islandID, const Vec3& chunkCoord, const Vec3& islandPosition,
                                                   const uint8_t* voxelData, uint32_t dataSize)
        { this->handleCompressedChunkReceived(islandID, chunkCoord, islandPosition, voxelData, dataSize); };

        client->onVoxelChangeReceived = [this](const VoxelChangeUpdate& update)
        { this->handleVoxelChangeReceived(update); };

        client->onEntityStateUpdate = [this](const EntityStateUpdate& update)
        { this->handleEntityStateUpdate(update); };
        
        // Setup fluid particle callbacks
        client->onFluidParticleSpawn = [this](const FluidParticleSpawnMessage& msg)
        { this->handleFluidParticleSpawn(msg); };
        
        client->onFluidParticleDespawn = [this](const FluidParticleDespawnMessage& msg)
        { this->handleFluidParticleDespawn(msg); };
    }
}

GameClient::~GameClient()
{
    // Clear global day/night controller pointer
    g_dayNightController = nullptr;
    
    shutdown();
}

bool GameClient::initialize(bool enableDebug)
{
    if (m_initialized)
    {
        std::cerr << "GameClient already initialized!" << std::endl;
        return false;
    }

    m_debugMode = enableDebug;

    // Initialize window and graphics
    if (!initializeWindow())
    {
        return false;
    }

    if (!initializeVulkan())
    {
        return false;
    }

    m_initialized = true;
    return true;
}

bool GameClient::connectToClientWorld(ClientWorld* clientWorld)
{
    if (!clientWorld)
    {
        std::cerr << "Cannot connect to null client world!" << std::endl;
        return false;
    }

    m_clientWorld = clientWorld;
    m_isRemoteClient = false;  // Local connection

    // **NEW: Connect physics system to island system for collision detection**
    if (clientWorld && clientWorld->getIslandSystem())
    {
        m_clientPhysics.setIslandSystem(clientWorld->getIslandSystem());
        // Mark chunks as client-side (need GPU upload)
        clientWorld->getIslandSystem()->setIsClient(true);
    }

    // Use calculated spawn position from world generation
    Vec3 playerSpawnPos = clientWorld->getPlayerSpawnPosition();
    m_playerController.setPosition(playerSpawnPos);

    return true;
}

bool GameClient::connectToRemoteServer(const std::string& serverAddress, uint16_t serverPort)
{
    if (!m_networkManager)
    {
        std::cerr << "Network manager not initialized!" << std::endl;
        return false;
    }

    // Initialize networking
    if (!m_networkManager->initializeNetworking())
    {
        std::cerr << "Failed to initialize networking!" << std::endl;
        return false;
    }

    // Connect to remote server
    if (!m_networkManager->joinServer(serverAddress, serverPort))
    {
        std::cerr << "Failed to connect to remote server!" << std::endl;
        return false;
    }

    m_isRemoteClient = true;

    return true;
}

bool GameClient::update(float deltaTime)
{
    PROFILE_SCOPE("GameClient::update");
    
    if (!m_initialized)
    {
        return false;
    }

    // Polling now occurs during window update at end of frame

    // Check if window should close
    if (shouldClose())
    {
        return false;
    }
    
    // Track frame time for FPS calculation
    m_lastFrameDeltaTime = deltaTime;

    // Update networking if remote client
    if (m_isRemoteClient && m_networkManager)
    {
        m_networkManager->update();
    }

    // Update client-side physics for smooth island movement
    if (m_clientWorld)
    {
        auto* islandSystem = m_clientWorld->getIslandSystem();
        if (islandSystem)
        {
            // Run client-side island physics between server updates
            // This provides smooth movement using server-provided velocities
            islandSystem->updateIslandPhysics(deltaTime);
        }
    }

    // Update day/night cycle for dynamic sun/lighting
    if (m_dayNightController)
    {
        m_dayNightController->update(deltaTime);
    }

    // Poll async mesh generation and upload when ready
    {
        PROFILE_SCOPE("AsyncMeshUpload");
        auto it = m_chunksWithPendingMeshes.begin();
        while (it != m_chunksWithPendingMeshes.end()) {
            if ((*it)->tryUploadPendingMesh()) {
                it = m_chunksWithPendingMeshes.erase(it); // Upload complete
            } else {
                ++it; // Still building
            }
        }
    }

    // Process input
    {
        PROFILE_SCOPE("processInput");
        processInput(deltaTime);
    }

    // Render frame
    {
        PROFILE_SCOPE("render");
        render();
    }

    // Swap buffers and poll events via wrapper
    {
        PROFILE_SCOPE("Window::update");
        if (m_window) m_window->update();
    }

    return true;
}

void GameClient::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    // Disconnect from game state
    m_clientWorld = nullptr;

    // Cleanup Vulkan renderers BEFORE VulkanContext
    if (m_vulkanCloudRenderer)
    {
        m_vulkanCloudRenderer.reset();
        std::cout << "VulkanCloudRenderer shutdown" << std::endl;
    }
    
    if (m_vulkanDeferred)
    {
        m_vulkanDeferred.reset();
        std::cout << "VulkanDeferred shutdown" << std::endl;
    }
    
    if (m_vulkanSkyRenderer)
    {
        m_vulkanSkyRenderer->shutdown();
        m_vulkanSkyRenderer.reset();
        std::cout << "VulkanSkyRenderer shutdown" << std::endl;
    }
    
    if (m_vulkanQuadRenderer)
    {
        m_vulkanQuadRenderer->shutdown();
        m_vulkanQuadRenderer.reset();
        g_vulkanQuadRenderer = nullptr;
        std::cout << "VulkanQuadRenderer shutdown" << std::endl;
    }
    
    if (m_vulkanModelRenderer)
    {
        m_vulkanModelRenderer->shutdown();
        m_vulkanModelRenderer.reset();
        g_vulkanModelRenderer = nullptr;
        std::cout << "VulkanModelRenderer shutdown" << std::endl;
    }
    
    if (m_vulkanBlockHighlighter)
    {
        m_vulkanBlockHighlighter->shutdown();
        m_vulkanBlockHighlighter.reset();
        std::cout << "VulkanBlockHighlighter shutdown" << std::endl;
    }
    
    // Cleanup ImGui BEFORE VulkanContext (it uses the descriptor pool)
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    // Cleanup VulkanContext (destroys descriptor pool AFTER ImGui is done)
    if (m_vulkanContext)
    {
        m_vulkanContext.reset();
        std::cout << "VulkanContext shutdown" << std::endl;
    }

    // Cleanup window
    if (m_window)
    {
        m_window->shutdown();
        m_window.reset();
    }

    // Terminate GLFW after window shutdown
    glfwTerminate();

    m_initialized = false;
}

bool GameClient::shouldClose() const
{
    return m_window && m_window->shouldClose();
}

void GameClient::processInput(float deltaTime)
{
    if (!m_window)
    {
        return;
    }

    processKeyboard(deltaTime);
    
    // Update player controller (handles movement, physics, and camera)
    if (m_clientWorld)
    {
        // Tell PlayerController if UI is blocking input
        bool uiBlocking = (m_periodicTableUI && m_periodicTableUI->isOpen());
        m_playerController.setUIBlocking(uiBlocking);
        
        // Process mouse input
        m_playerController.processMouse(m_window->getHandle());
        
        // Update player controller (physics and camera)
        m_playerController.update(m_window->getHandle(), deltaTime, m_clientWorld->getIslandSystem(), &m_clientPhysics);
        
        // Send movement to server if remote client
        if (m_isRemoteClient && m_networkManager)
        {
            Vec3 pos = m_playerController.getPosition();
            Vec3 vel = m_playerController.getVelocity();
            m_networkManager->sendPlayerMovement(pos, vel, deltaTime);
        }
    }

    // Process block interaction
    if (m_clientWorld)
    {
        processBlockInteraction(deltaTime);
    }
}

void GameClient::render()
{
    PROFILE_SCOPE("GameClient::render");
    
    // Vulkan render path
    renderVulkan();
    
    // Render UI
    {
        PROFILE_SCOPE("renderUI");
        renderUI();
    }
}

bool GameClient::initializeWindow()
{
    // Use the Window wrapper for all window/context handling
    m_window = std::make_unique<Engine::Core::Window>();
    if (!m_window->initialize(m_windowWidth, m_windowHeight, "MMORPG Engine - Client", m_debugMode))
    {
        std::cerr << "Failed to initialize window!" << std::endl;
        return false;
    }

    // Set resize callback to update camera/aspect
    m_window->setResizeCallback([this](int width, int height) { this->onWindowResize(width, height); });
    
    // Set scroll callback for reticle distance adjustment
    m_window->setScrollCallback([this](double /*xoffset*/, double yoffset) {
        // Adjust reticle distance (yoffset is positive for scroll up, negative for scroll down)
        const float scrollSensitivity = 0.5f;
        m_inputState.reticleDistance += static_cast<float>(yoffset) * scrollSensitivity;
        
        // Clamp to reasonable range
        m_inputState.reticleDistance = glm::clamp(m_inputState.reticleDistance, 1.0f, 50.0f);
    });

    // Set up mouse capture on underlying GLFW window
    glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    return true;
}

bool GameClient::initializeCommonSystems()
{
    // Initialize texture manager (needed by all renderers)
    extern TextureManager* g_textureManager;
    if (!g_textureManager)
    {
        g_textureManager = new TextureManager();
        if (!g_textureManager->initialize())
        {
            std::cerr << "❌ Failed to initialize TextureManager!" << std::endl;
            return false;
        }
    }
    
    // Initialize HUD overlay
    m_hud = std::make_unique<HUD>();
    
    // Initialize Periodic Table UI
    m_periodicTableUI = std::make_unique<PeriodicTableUI>();
    
    return true;
}

bool GameClient::initializeVulkan()
{
    std::cout << "🔷 Initializing Vulkan rendering backend..." << std::endl;
    
    // Initialize common systems (texture manager, mesh queue, UI)
    if (!initializeCommonSystems())
    {
        return false;
    }
    
    // Initialize Vulkan context (device, swapchain, etc.)
    m_vulkanContext = std::make_unique<VulkanContext>();
    if (!m_vulkanContext->init(m_window->getHandle(), true /* enable validation */))
    {
        std::cerr << "❌ Failed to initialize VulkanContext!" << std::endl;
        return false;
    }
    std::cout << "✅ VulkanContext initialized" << std::endl;
    
    // Initialize Vulkan quad renderer (replaces InstancedQuadRenderer)
    m_vulkanQuadRenderer = std::make_unique<VulkanQuadRenderer>();
    if (!m_vulkanQuadRenderer->initialize(m_vulkanContext.get()))
    {
        std::cerr << "❌ Failed to initialize VulkanQuadRenderer!" << std::endl;
        return false;
    }
    std::cout << "✅ VulkanQuadRenderer initialized - Vulkan MDI rendering ready!" << std::endl;
    
    // Set global pointer for chunk mesh uploads
    g_vulkanQuadRenderer = m_vulkanQuadRenderer.get();
    
    // Initialize Vulkan sky renderer
    m_vulkanSkyRenderer = std::make_unique<VulkanSkyRenderer>();
    if (!m_vulkanSkyRenderer->initialize(m_vulkanContext.get()))
    {
        std::cerr << "⚠️  Warning: VulkanSkyRenderer failed to initialize (VMA buffer allocation issue)" << std::endl;
        std::cerr << "   Continuing without skybox rendering..." << std::endl;
        m_vulkanSkyRenderer.reset();  // Clear failed renderer
    } else {
        std::cout << "✅ VulkanSkyRenderer initialized" << std::endl;
    }
    
    // Initialize Vulkan block highlighter
    m_vulkanBlockHighlighter = std::make_unique<VulkanBlockHighlighter>();
    if (!m_vulkanBlockHighlighter->initialize(m_vulkanContext.get()))
    {
        std::cerr << "⚠️  Warning: VulkanBlockHighlighter failed to initialize" << std::endl;
        m_vulkanBlockHighlighter.reset();
    } else {
        std::cout << "✅ VulkanBlockHighlighter initialized" << std::endl;
    }
    
    // Initialize Vulkan cloud renderer
    m_vulkanCloudRenderer = std::make_unique<VulkanCloudRenderer>();
    if (!m_vulkanCloudRenderer->initialize(m_vulkanContext.get(), m_windowWidth, m_windowHeight))
    {
        std::cerr << "⚠️  Warning: VulkanCloudRenderer failed to initialize" << std::endl;
        m_vulkanCloudRenderer.reset();
    } else {
        std::cout << "✅ VulkanCloudRenderer initialized" << std::endl;
    }
    
    // Initialize Vulkan deferred renderer (G-buffer + lighting pass)
    m_vulkanDeferred = std::make_unique<VulkanDeferred>();
    if (!m_vulkanDeferred->initialize(m_vulkanContext->getDevice(), 
                                      m_vulkanContext->getAllocator(),
                                      m_vulkanContext->pipelineCache,
                                      m_vulkanContext->getSwapchainFormat(),
                                      m_windowWidth, m_windowHeight,
                                      m_vulkanContext->getGraphicsQueue(),
                                      m_vulkanContext->getCommandPool(),
                                      m_vulkanContext.get()))
    {
        std::cerr << "❌ Failed to initialize VulkanDeferred!" << std::endl;
        return false;
    }
    std::cout << "✅ VulkanDeferred initialized - Shadow maps + lighting pass ready!" << std::endl;
    
    // Bind shadow maps and cloud noise to lighting pass
    if (m_vulkanCloudRenderer) {
        if (!m_vulkanDeferred->bindLightingTextures(m_vulkanCloudRenderer->getNoiseTextureView())) {
            std::cerr << "❌ Failed to bind lighting textures!" << std::endl;
            return false;
        }
        std::cout << "✅ Cascade light maps bound to lighting pass" << std::endl;
    }
    
    // Initialize Vulkan model renderer (for GLB models) - requires G-Buffer render pass
    m_vulkanModelRenderer = std::make_unique<VulkanModelRenderer>();
    if (!m_vulkanModelRenderer->initialize(m_vulkanContext.get()))
    {
        std::cerr << "❌ Failed to initialize VulkanModelRenderer!" << std::endl;
        return false;
    }
    std::cout << "✅ VulkanModelRenderer initialized - GLB model instancing ready!" << std::endl;
    g_vulkanModelRenderer = m_vulkanModelRenderer.get();
    
    // Initialize ImGui with Vulkan backend
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    ImGui::StyleColorsDark();
    
    // Initialize GLFW backend for Vulkan
    ImGui_ImplGlfw_InitForVulkan(m_window->getHandle(), true);
    
    // Initialize Vulkan backend for ImGui with dynamic rendering
    VkFormat swapchainFormat = m_vulkanContext->getSwapchainFormat();
    VkPipelineRenderingCreateInfo imguiRenderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    imguiRenderingInfo.colorAttachmentCount = 1;
    imguiRenderingInfo.pColorAttachmentFormats = &swapchainFormat;
    imguiRenderingInfo.depthAttachmentFormat = m_vulkanContext->getDepthFormat();
    
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = m_vulkanContext->instance;
    init_info.PhysicalDevice = m_vulkanContext->physicalDevice;
    init_info.Device = m_vulkanContext->device;
    init_info.QueueFamily = m_vulkanContext->graphicsQueueFamily;
    init_info.Queue = m_vulkanContext->graphicsQueue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = m_vulkanContext->descriptorPool;
    init_info.UseDynamicRendering = true;
    init_info.PipelineRenderingCreateInfo = imguiRenderingInfo;
    init_info.MinImageCount = 2;
    init_info.ImageCount = static_cast<uint32_t>(m_vulkanContext->swapchainImages.size());
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;
    
    if (!ImGui_ImplVulkan_Init(&init_info))
    {
        std::cerr << "❌ Failed to initialize ImGui Vulkan backend!" << std::endl;
        return false;
    }
    
    std::cout << "✅ Vulkan rendering backend initialized!" << std::endl;
    return true;
}

void GameClient::renderVulkan()
{
    // Begin frame (without starting render pass yet)
    uint32_t imageIndex = 0;
    if (!m_vulkanContext->beginFrame(imageIndex, false))
    {
        return; // Swapchain out of date, wait for resize
    }
    
    VkCommandBuffer cmd = m_vulkanContext->getCurrentCommandBuffer();
    
    // Calculate camera matrices
    float aspect = static_cast<float>(m_windowWidth) / static_cast<float>(m_windowHeight);
    glm::mat4 projectionMatrix = m_playerController.getCamera().getProjectionMatrix(aspect);
    glm::mat4 viewMatrix = m_playerController.getCamera().getViewMatrix();
    glm::mat4 viewProjection = projectionMatrix * viewMatrix;
    
    // Update island transforms for Vulkan renderer (islands drift each frame)
    if (m_clientWorld)
    {
        auto* islandSystem = m_clientWorld->getIslandSystem();
        if (islandSystem)
        {
            const auto& islands = islandSystem->getIslands();
            for (const auto& [islandID, island] : islands)
            {
                if (m_vulkanQuadRenderer)
                {
                    m_vulkanQuadRenderer->updateIslandTransform(islandID, island.getTransformMatrix());
                }
                if (m_vulkanModelRenderer)
                {
                    m_vulkanModelRenderer->updateIslandTransform(islandID, island.getTransformMatrix());
                }
            }
        }
    }
    
    // Process pending mesh uploads (batched for performance)
    if (m_vulkanQuadRenderer)
    {
        m_vulkanQuadRenderer->processPendingUploads();
    }
    
    // Update dynamic buffers BEFORE render pass (vkCmdUpdateBuffer cannot be called inside render pass)
    if (m_vulkanQuadRenderer)
    {
        m_vulkanQuadRenderer->updateDynamicBuffers(cmd, viewProjection);
    }
    
    // Get sun/moon data for lighting
    float timeOfDay = m_dayNightController ? m_dayNightController->getTimeOfDay() : 12.0f;
    Vec3 sunDir = m_dayNightController ? m_dayNightController->getSunDirection() : Vec3(0.0f, -1.0f, 0.0f);
    Vec3 moonDir = m_dayNightController ? m_dayNightController->getMoonDirection() : Vec3(0.0f, 1.0f, 0.0f);
    float sunIntensity = m_dayNightController ? m_dayNightController->getSunIntensity() : 1.0f;
    float moonIntensity = m_dayNightController ? m_dayNightController->getMoonIntensity() : 0.0f;
    
    glm::vec3 sunDirGLM(sunDir.x, sunDir.y, sunDir.z);
    glm::vec3 moonDirGLM(moonDir.x, moonDir.y, moonDir.z);
    
    // ========================================
    // PASS 0: SHADOW MAPS (Cascaded Light Maps)
    // ========================================
    // Calculate cascade matrices from camera frustum and sun direction
    VulkanDeferred::CascadeUniforms cascades{};
    glm::vec3 camPos(m_playerController.getCamera().position.x,
                     m_playerController.getCamera().position.y,
                     m_playerController.getCamera().position.z);
    
    // Cascade split distances (near to far)
    float cascadeSplits[4] = {50.0f, 200.0f, 800.0f, 3000.0f};
    float orthoSizes[4] = {100.0f, 400.0f, 1600.0f, 6000.0f};
    
    // Depth ranges match ortho sizes for resolution efficiency
    // Near cascades only render geometry within their effective range
    // Far cascades capture distant geometry that near cascades miss
    float depthRanges[4] = {
        orthoSizes[0] * 1.5f,  // Near sun: ~150 blocks depth
        orthoSizes[1] * 1.5f,  // Far sun: ~600 blocks depth
        orthoSizes[2] * 1.5f,  // Near moon: ~2400 blocks depth
        orthoSizes[3] * 1.5f   // Far moon: ~9000 blocks depth
    };
    
    for (int i = 0; i < 4; i++) {
        glm::vec3 lightDir = (i < 2) ? sunDirGLM : moonDirGLM;
        float splitDist = cascadeSplits[i];
        float orthoSize = orthoSizes[i];
        float depthRange = depthRanges[i];
        
        // Light view: look from light direction toward camera
        glm::vec3 lightPos = camPos - lightDir * (splitDist * 0.5f);
        glm::mat4 lightView = glm::lookAt(lightPos, camPos, glm::vec3(0, 1, 0));
        
        // Orthographic projection with depth range matching ortho size
        // This prevents near cascades from wasting resolution on distant geometry
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, 
                                         -depthRange, depthRange);
        
        cascades.cascadeVP[i] = lightProj * lightView;
        cascades.cascadeOrthoSizes[i] = orthoSize;
    }
    cascades.lightTexel = glm::vec4(1.0f / 4096.0f, 0, 0, 0);
    
    // Render shadow maps from sun/moon POV
    if (m_vulkanDeferred && m_vulkanQuadRenderer) {
        auto& shadowMap = m_vulkanDeferred->getShadowMap();
        
        // Render each cascade
        for (int i = 0; i < 4; i++) {
            shadowMap.beginCascadeRender(cmd, i);
            
            // Render geometry from light's perspective
            m_vulkanQuadRenderer->renderDepthOnly(cmd, cascades.cascadeVP[i]);
            
            shadowMap.endCascadeRender(cmd, i);
        }
        
        // Transition shadow maps for shader reading
        shadowMap.transitionForShaderRead(cmd);
    }
    
    // ========================================
    // PASS 1: G-BUFFER (Deferred Geometry Pass)
    // ========================================
    if (m_vulkanDeferred)
    {
        // Depth layout cycle: Frame 0 needs explicit UNDEFINED->ATTACHMENT transition before G-buffer
        // Subsequent frames: READ_ONLY->ATTACHMENT transition before G-buffer
        static bool firstFrame = true;
        
        // Debug: Check what layout tracker thinks depth is in
        auto& tracker = VulkanLayoutTracker::getInstance();
        VkImageLayout currentTrackedLayout = tracker.getCurrentLayout(m_vulkanContext->getDepthImage());
        if (tracker.m_verbose) {
            printf("[LayoutTracker] GameClient before G-buffer: firstFrame=%d, tracker says depth is %s\n",
                   firstFrame, tracker.getLayoutName(currentTrackedLayout));
        }
        
        if (firstFrame) {
            // Frame 0: Explicit barrier to transition UNDEFINED -> ATTACHMENT before G-buffer
            // Cannot use UNDEFINED in VkRenderingAttachmentInfo (Vulkan spec VUID-VkRenderingAttachmentInfo-imageView-06135)
            VulkanLayoutTracker::getInstance().recordTransition(
                m_vulkanContext->getDepthImage(),
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                "GameClient: Frame 0 (UNDEFINED->ATTACHMENT before G-buffer)");
            
            VkImageMemoryBarrier depthToAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            depthToAttachment.srcAccessMask = 0;  // No prior access
            depthToAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthToAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depthToAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthToAttachment.image = m_vulkanContext->getDepthImage();
            depthToAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthToAttachment.subresourceRange.levelCount = 1;
            depthToAttachment.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &depthToAttachment);
            firstFrame = false;
        } else {
            // Frame 1+: Explicit barrier to transition READ_ONLY -> ATTACHMENT before G-buffer
            VulkanLayoutTracker::getInstance().recordTransition(
                m_vulkanContext->getDepthImage(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                "GameClient: Before G-buffer (READ_ONLY->ATTACHMENT)");
            
            VkImageMemoryBarrier depthToAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            depthToAttachment.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            depthToAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthToAttachment.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthToAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthToAttachment.image = m_vulkanContext->getDepthImage();
            depthToAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthToAttachment.subresourceRange.levelCount = 1;
            depthToAttachment.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &depthToAttachment);
        }
        
        // G-buffer always receives depth in ATTACHMENT layout (transitioned above)
        m_vulkanDeferred->beginGeometryPass(cmd, m_vulkanContext->getDepthImage(), m_vulkanContext->getDepthImageView(), 
                                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        
        // Render voxels to G-buffer
        if (m_vulkanQuadRenderer)
        {
            m_vulkanQuadRenderer->renderToGBuffer(cmd, viewProjection, viewMatrix);
        }
        
        // Render GLB models to G-buffer
        if (m_vulkanModelRenderer)
        {
            m_vulkanModelRenderer->renderToGBuffer(cmd, viewProjection, viewMatrix);
        }
        
        m_vulkanDeferred->endGeometryPass(cmd);
        
        // Transition depth to read-only for lighting pass (descriptor read)
        VulkanLayoutTracker::getInstance().recordTransition(
            m_vulkanContext->getDepthImage(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            "GameClient: After G-buffer");
        
        VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthBarrier.image = m_vulkanContext->getDepthImage();
        depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthBarrier.subresourceRange.levelCount = 1;
        depthBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
        
        // Update depth descriptor to match new layout (first time only writes descriptor)
        static bool depthDescriptorWritten = false;
        if (!depthDescriptorWritten) {
            VulkanLayoutTracker::getInstance().recordDescriptorWrite(
                m_vulkanContext->getDepthImageView(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                "GameClient: FIRST depth descriptor write");
            depthDescriptorWritten = true;
        }
        
        VkDescriptorImageInfo depthInfo = {};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depthInfo.imageView = m_vulkanContext->getDepthImageView();
        depthInfo.sampler = m_vulkanDeferred->getGBufferSampler();
        
        VkWriteDescriptorSet depthWrite = {};
        depthWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        depthWrite.dstSet = m_vulkanDeferred->getLightingDescriptorSet();
        depthWrite.dstBinding = 4;
        depthWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depthWrite.descriptorCount = 1;
        depthWrite.pImageInfo = &depthInfo;
        
        vkUpdateDescriptorSets(m_vulkanContext->device, 1, &depthWrite, 0, nullptr);
    }
    
    // ========================================
    // PASS 1.5: LIGHTING TO HDR (opaque geometry only)
    // ========================================
    if (m_vulkanDeferred)
    {
        VulkanDeferred::LightingParams lightingParams;
        lightingParams.sunDirection = glm::vec4(sunDirGLM, sunIntensity);
        lightingParams.moonDirection = glm::vec4(moonDirGLM, moonIntensity);
        lightingParams.sunColor = glm::vec4(1.0f, 0.95f, 0.8f, 1.0f);
        lightingParams.moonColor = glm::vec4(0.6f, 0.7f, 1.0f, 1.0f);
        lightingParams.cameraPos = glm::vec4(
            m_playerController.getCamera().position.x,
            m_playerController.getCamera().position.y,
            m_playerController.getCamera().position.z,
            timeOfDay
        );
        
        VkImageView cloudNoise = m_vulkanCloudRenderer ? m_vulkanCloudRenderer->getNoiseTextureView() : VK_NULL_HANDLE;
        m_vulkanDeferred->renderLightingToHDR(cmd, lightingParams, cascades, cloudNoise);
    }
    
    // ========================================
    // PASS 1.6: SSR COMPUTE (raymarches lit HDR buffer for reflections)
    // Runs AFTER lighting to capture lit scene colors
    // ========================================
    if (m_vulkanDeferred)
    {
        m_vulkanDeferred->computeSSR(cmd, viewMatrix, projectionMatrix);
    }
    
    // ========================================
    // PASS 2: FINAL COMPOSITION (Sky + HDR + SSR)
    // ========================================
    
    // Depth remains in READ_ONLY_OPTIMAL for depth testing during composition
    // Start swapchain render pass for final composition
    m_vulkanContext->beginRenderPass(cmd, imageIndex);
    
    // Render skybox first
    if (m_vulkanSkyRenderer)
    {
        m_vulkanSkyRenderer->render(cmd, sunDirGLM, sunIntensity, moonDirGLM, moonIntensity,
                                   viewMatrix, projectionMatrix, timeOfDay);
    }
    
    // Render volumetric clouds (before terrain, so terrain occludes them)
    if (m_vulkanCloudRenderer)
    {
        VulkanCloudRenderer::CloudParams cloudParams;
        cloudParams.sunDirection = sunDirGLM;
        cloudParams.sunIntensity = sunIntensity;
        cloudParams.cameraPosition = m_playerController.getCamera().position;
        cloudParams.timeOfDay = timeOfDay;
        cloudParams.cloudCoverage = 0.5f;
        cloudParams.cloudDensity = 0.5f;
        cloudParams.cloudSpeed = 0.5f;
        
        // Depth view is from VulkanContext (D32_SFLOAT), not G-buffer
        VkImageView depthView = m_vulkanContext->getDepthImageView();
        m_vulkanCloudRenderer->render(cmd, depthView,
                                     viewMatrix, projectionMatrix, cloudParams);
    }
    
    // Composite HDR + SSR to swapchain (writes opaque terrain OVER clouds)
    if (m_vulkanDeferred)
    {
        VkViewport viewport = {0.0f, 0.0f, (float)m_vulkanDeferred->getWidth(), (float)m_vulkanDeferred->getHeight(), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        
        VkRect2D scissor = {{0, 0}, {m_vulkanDeferred->getWidth(), m_vulkanDeferred->getHeight()}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        
        m_vulkanDeferred->compositeToSwapchain(cmd, m_playerController.getCamera().position);
    }
    
    // ========================================
    // PASS 3: FORWARD TRANSPARENT WATER (industry standard)
    // Renders after composition with alpha blending
    // ========================================
    if (m_vulkanModelRenderer && m_vulkanDeferred)
    {
        VkImageView depthTexture = m_vulkanContext->getDepthImageView();
        VkImageView hdrTexture = m_vulkanDeferred->getHDRView();
        VkSampler sampler = m_vulkanDeferred->getGBufferSampler();
        m_vulkanModelRenderer->renderForward(cmd, viewProjection, m_playerController.getCamera().position, depthTexture, hdrTexture, sampler);
    }
    
    // Render reticle wireframe - snaps to hit block if valid target found
    if (m_vulkanBlockHighlighter)
    {
        const Camera& camera = m_playerController.getCamera();
        
        // Render Lock A (persistent) - in island-local coordinates
        if (m_inputState.hasLockA)
        {
            auto& islands = m_clientWorld->getIslandSystem()->getIslands();
            auto it = islands.find(m_inputState.lockAIslandID);
            if (it != islands.end())
            {
                const FloatingIsland& island = it->second;
                glm::mat4 islandTransform = island.getTransformMatrix();
                m_vulkanBlockHighlighter->render(cmd, m_inputState.lockAPos, islandTransform, viewProjection);
            }
        }
        
        // Render Lock B (persistent) - in island-local coordinates
        if (m_inputState.hasLockB)
        {
            auto& islands = m_clientWorld->getIslandSystem()->getIslands();
            auto it = islands.find(m_inputState.lockBIslandID);
            if (it != islands.end())
            {
                const FloatingIsland& island = it->second;
                glm::mat4 islandTransform = island.getTransformMatrix();
                m_vulkanBlockHighlighter->render(cmd, m_inputState.lockBPos, islandTransform, viewProjection);
            }
        }
        
        // Render current reticle (only if no locks exist yet)
        if (!m_inputState.hasLockA || !m_inputState.hasLockB)
        {
            if (m_inputState.reticleHasTarget)
            {
                // Snap to the hit block position on its island
                auto& islands = m_clientWorld->getIslandSystem()->getIslands();
                auto it = islands.find(m_inputState.reticleIslandID);
                if (it != islands.end())
                {
                    const FloatingIsland& island = it->second;
                    glm::mat4 islandTransform = island.getTransformMatrix();
                    m_vulkanBlockHighlighter->render(cmd, m_inputState.cachedTargetBlock.localBlockPos, islandTransform, viewProjection);
                }
            }
            else
            {
                // No target - show reticle at freeplace position with yaw rotation
                glm::mat4 reticleTransform = glm::translate(glm::mat4(1.0f), 
                                                             glm::vec3(m_inputState.reticleWorldPos.x, 
                                                                      m_inputState.reticleWorldPos.y, 
                                                                      m_inputState.reticleWorldPos.z));
                reticleTransform = glm::rotate(reticleTransform, 
                                               glm::radians(-camera.yaw), 
                                               glm::vec3(0.0f, 1.0f, 0.0f));
                reticleTransform = glm::translate(reticleTransform, glm::vec3(-0.5f, -0.5f, -0.5f));
                
                Vec3 origin(0, 0, 0);
                m_vulkanBlockHighlighter->render(cmd, origin, reticleTransform, viewProjection);
            }
        }
    }
    
    // Render ImGui
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }
    
    // End dynamic rendering before copy
    vkCmdEndRendering(cmd);
    
    // Transition swapchain image to PRESENT_SRC layout
    VkImageMemoryBarrier presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstAccessMask = 0;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = m_vulkanContext->getSwapchainImage(imageIndex);
    presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &presentBarrier);
    
    // End frame and present (don't end render pass again)
    m_vulkanContext->endFrame(imageIndex, false);
}

void GameClient::processKeyboard(float deltaTime)
{
    (void)deltaTime;
    
    // Tab key - toggle periodic table UI
    {
        static bool tabKeyPressed = false;
        bool isTabPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_TAB) == GLFW_PRESS;
        
        if (isTabPressed && !tabKeyPressed) {
            m_periodicTableUI->toggle();
            
            // Toggle mouse cursor and camera control
            if (m_periodicTableUI->isOpen()) {
                glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                std::cout << "Periodic table opened (mouse visible)" << std::endl;
            } else {
                glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                std::cout << "Periodic table closed (mouse captured)" << std::endl;
            }
        }
        
        tabKeyPressed = isTabPressed;
    }
    
    // NEW: Element-based crafting system (keys 1-9 add elements to queue)
    // Skip if periodic table is open (it handles key input itself)
    if (!m_periodicTableUI->isOpen())
    {
        static bool numberKeysPressed[10] = {false};  // 0-9
        
        // Keys 1-9: Add elements from customizable hotbar
        for (int i = 0; i < 9; ++i)
        {
            int key = GLFW_KEY_1 + i;  // GLFW_KEY_1 through GLFW_KEY_9
            bool isPressed = glfwGetKey(m_window->getHandle(), key) == GLFW_PRESS;
            
            if (isPressed && !numberKeysPressed[i])
            {
                // Auto-unlock previous recipe when starting a new element sequence
                if (m_elementQueue.isEmpty() && m_lockedRecipe) {
                    m_lockedRecipe = nullptr;
                    std::cout << "Previous recipe unlocked (starting new craft)" << std::endl;
                }
                
                // Add element from customizable hotbar
                Element elem = m_hotbarElements[i];
                m_elementQueue.addElement(elem);
                
                // Check if this matches a recipe
                auto& recipeSystem = ElementRecipeSystem::getInstance();
                const BlockRecipe* recipe = recipeSystem.matchRecipe(m_elementQueue);
                
                if (recipe) {
                    std::cout << "✓ Recipe matched: " << recipe->name << " (" << recipe->formula << ")" << std::endl;
                } else {
                    std::cout << "Element added: " << ElementRecipeSystem::getElementSymbol(elem) 
                              << " (Queue: " << m_elementQueue.toFormula() << ")" << std::endl;
                }
            }
            
            numberKeysPressed[i] = isPressed;
        }
        
        // Key 0: Clear element queue
        bool isZeroPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_0) == GLFW_PRESS;
        if (isZeroPressed && !numberKeysPressed[9])
        {
            m_elementQueue.clear();
            m_lockedRecipe = nullptr;
            std::cout << "Element queue cleared" << std::endl;
        }
        numberKeysPressed[9] = isZeroPressed;
    }

    // Debug collision info (press C to debug collision system)
    static bool wasDebugKeyPressed = false;
    bool isDebugKeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_C) == GLFW_PRESS;
    
    if (isDebugKeyPressed && !wasDebugKeyPressed)
    {
        m_clientPhysics.debugCollisionInfo(m_playerController.getCamera().position, 0.5f);
    }
    wasDebugKeyPressed = isDebugKeyPressed;
    
    // Toggle HUD debug info (press F3)
    static bool wasF3KeyPressed = false;
    bool isF3KeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_F3) == GLFW_PRESS;
    
    if (isF3KeyPressed && !wasF3KeyPressed && m_hud)
    {
        m_hud->toggleDebugInfo();
    }
    wasF3KeyPressed = isF3KeyPressed;
    
    // Toggle noclip mode (press N for debug flying)
    static bool wasNoclipKeyPressed = false;
    bool isNoclipKeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_N) == GLFW_PRESS;
    
    if (isNoclipKeyPressed && !wasNoclipKeyPressed)
    {
        m_playerController.setNoclipMode(!m_playerController.isNoclipMode());
        std::cout << (m_playerController.isNoclipMode() ? "🕊️ Noclip enabled (flying)" : "🚶 Physics enabled (walking)") << std::endl;
    }
    wasNoclipKeyPressed = isNoclipKeyPressed;

    // Toggle camera smoothing (press L to see raw physics - helpful for debugging)
    static bool wasSmoothingKeyPressed = false;
    bool isSmoothingKeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_L) == GLFW_PRESS;
    
    if (isSmoothingKeyPressed && !wasSmoothingKeyPressed)
    {
        m_playerController.setCameraSmoothing(!m_playerController.isCameraSmoothingEnabled());
        std::cout << (m_playerController.isCameraSmoothingEnabled() ? "📹 Camera smoothing enabled (smooth)" : "📹 Camera smoothing disabled (raw physics)") << std::endl;
    }
    wasSmoothingKeyPressed = isSmoothingKeyPressed;

    // Toggle piloting (press E to pilot the island/vehicle you're standing on)
    static bool wasEKeyPressed = false;
    bool isEKeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_E) == GLFW_PRESS;
    
    if (isEKeyPressed && !wasEKeyPressed)
    {
        m_playerController.setPiloting(!m_playerController.isPiloting(), m_playerController.getPilotedIslandID());
        if (m_playerController.isPiloting())
        {
            std::cout << "🚀 Piloting ENABLED - Arrows: forward/back/rotate, Space/Shift: up/down" << std::endl;
        }
        else
        {
            std::cout << "🚶 Piloting DISABLED - normal movement" << std::endl;
        }
    }
    wasEKeyPressed = isEKeyPressed;

    // Post-processing controls
    // Apply piloting controls (arrow keys for movement and rotation)
    // Send inputs to server instead of directly modifying island
    if (m_playerController.isPiloting() && m_playerController.getPilotedIslandID() != 0)
    {
        uint32_t pilotedIslandID = m_playerController.getPilotedIslandID();
        
        // Gather input values
        float thrustY = 0.0f;
        float rotationYaw = 0.0f;
        
        // Vertical thrust (space/shift)
        if (glfwGetKey(m_window->getHandle(), GLFW_KEY_SPACE) == GLFW_PRESS)
        {
            thrustY += 1.0f;
        }
        if (glfwGetKey(m_window->getHandle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        {
            thrustY -= 1.0f;
        }
        
        // Rotation (yaw only - left/right arrows)
        if (glfwGetKey(m_window->getHandle(), GLFW_KEY_LEFT) == GLFW_PRESS)
        {
            rotationYaw = 1.0f;  // Rotate left
        }
        else if (glfwGetKey(m_window->getHandle(), GLFW_KEY_RIGHT) == GLFW_PRESS)
        {
            rotationYaw = -1.0f; // Rotate right
        }
        
        // Send piloting input to server (if connected)
        if (m_networkManager && m_networkManager->getClient() && m_networkManager->getClient()->isConnected())
        {
            m_networkManager->getClient()->sendPilotingInput(pilotedIslandID, thrustY, rotationYaw);
        }
    }

    // Exit
    if (glfwGetKey(m_window->getHandle(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        m_window->setShouldClose(true);
    }
}

void GameClient::processBlockInteraction(float deltaTime)
{
    if (!m_clientWorld)
    {
        return;
    }

    // Update reticle position (moves along camera forward vector)
    const Camera& camera = m_playerController.getCamera();
    m_inputState.reticleWorldPos = camera.position + camera.front * m_inputState.reticleDistance;
    
    // Raycast from reticle toward camera to find first solid block
    auto* islandSystem = m_clientWorld->getIslandSystem();
    Vec3 reticlePos(m_inputState.reticleWorldPos.x, m_inputState.reticleWorldPos.y, m_inputState.reticleWorldPos.z);
    Vec3 toCameraDir = (camera.position - reticlePos).normalized();
    
    RayHit hit = VoxelRaycaster::raycast(reticlePos, toCameraDir, m_inputState.reticleDistance, islandSystem);
    
    m_inputState.reticleHasTarget = false;
    if (hit.hit)
    {
        // Found solid block - snap reticle to it for breaking
        m_inputState.reticleHasTarget = true;
        m_inputState.reticleIslandID = hit.islandID;
        m_inputState.cachedTargetBlock = hit;
        
        // For placement: find first air block from hit toward camera
        for (int step = 0; step < 10; ++step)
        {
            Vec3 testPos = hit.localBlockPos + toCameraDir * static_cast<float>(step);
            Vec3 voxelPos(std::floor(testPos.x), std::floor(testPos.y), std::floor(testPos.z));
            uint8_t voxel = islandSystem->getVoxelFromIsland(hit.islandID, voxelPos);
            
            if (voxel == 0) // Found air
            {
                m_inputState.reticleSnapPos = voxelPos;
                break;
            }
        }
    }
    else
    {
        m_inputState.cachedTargetBlock = RayHit();
    }

    bool leftClick = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool rightClick = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    bool middleClick = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

    // ==========================================
    // HOLD DETECTION FOR GEOMETRY MODE
    // ==========================================
    // Detect button state changes FIRST (before modifying timers)
    bool leftPressed = leftClick && !m_inputState.leftMouseWasPressed;
    bool leftReleased = !leftClick && m_inputState.leftMouseWasPressed;
    bool rightPressed = rightClick && !m_inputState.rightMouseWasPressed;
    bool rightReleased = !rightClick && m_inputState.rightMouseWasPressed;
    
    // Capture hold duration on release (before reset)
    static float leftReleaseDuration = 0.0f;
    static float rightReleaseDuration = 0.0f;
    
    if (leftReleased)
    {
        leftReleaseDuration = m_inputState.leftHoldTimer;
    }
    
    if (rightReleased)
    {
        rightReleaseDuration = m_inputState.rightHoldTimer;
    }
    
    // Track how long each button has been held
    if (leftClick)
    {
        m_inputState.leftHoldTimer += deltaTime;
    }
    else
    {
        m_inputState.leftHoldTimer = 0.0f;
    }
    
    if (rightClick)
    {
        m_inputState.rightHoldTimer += deltaTime;
    }
    else
    {
        m_inputState.rightHoldTimer = 0.0f;
    }

    // Check if currently holding
    bool leftIsHold = leftClick && m_inputState.leftHoldTimer >= m_inputState.holdThreshold;
    bool rightIsHold = rightClick && m_inputState.rightHoldTimer >= m_inputState.holdThreshold;
    
    // Debug hold state
    static bool debugHoldOnce = false;
    if (leftIsHold && !debugHoldOnce) {
        std::cout << "🔵 LMB HOLD DETECTED (timer=" << m_inputState.leftHoldTimer << " threshold=" << m_inputState.holdThreshold << ")" << std::endl;
        std::cout << "  Lock A: " << m_inputState.hasLockA << " Lock B: " << m_inputState.hasLockB << std::endl;
        debugHoldOnce = true;
    }
    if (!leftClick) debugHoldOnce = false;

    // ==========================================
    // MMB - CLEAR ALL LOCKS
    // ==========================================
    static bool middleWasPressed = false;
    if (middleClick && !middleWasPressed)
    {
        m_inputState.hasLockA = false;
        m_inputState.hasLockB = false;
        std::cout << "🗑️  All locks cleared" << std::endl;
    }
    middleWasPressed = middleClick;

    // ==========================================
    // LEFT CLICK/HOLD - INSTANT BREAK OR LOCK A OR AUTO-MINE
    // ==========================================
    // Click (release < 80ms) = instant break block
    // Hold (>= 80ms) = create Lock A (geometry mode) OR auto-mine if both locks exist
    
    // Handle LMB hold to create Lock A (only if Lock A doesn't exist)
    static bool lockACreated = false;
    if (leftIsHold && !m_inputState.hasLockA && !lockACreated && m_inputState.reticleHasTarget)
    {
        m_inputState.hasLockA = true;
        m_inputState.lockAPos = m_inputState.cachedTargetBlock.localBlockPos;
        m_inputState.lockAIslandID = m_inputState.cachedTargetBlock.islandID;
        lockACreated = true;
        std::cout << "🔒 Lock A created at " << m_inputState.lockAPos.x << ", " 
                  << m_inputState.lockAPos.y << ", " << m_inputState.lockAPos.z << std::endl;
    }
    
    // Auto-mine mode when both locks exist and LMB held
    static float autoMineTimer = 0.0f;
    static bool autoMineDebugOnce = false;
    if (leftIsHold && m_inputState.hasLockA && m_inputState.hasLockB && 
        m_inputState.lockAIslandID == m_inputState.lockBIslandID)
    {
        if (!autoMineDebugOnce) {
            std::cout << "🔥 AUTO-MINE MODE ACTIVATED! Timer starting..." << std::endl;
            autoMineDebugOnce = true;
        }
        autoMineTimer += deltaTime;
        std::cout << "  Auto-mine timer: " << autoMineTimer << " / " << m_inputState.autoOperationRate << std::endl;
        
        if (autoMineTimer >= m_inputState.autoOperationRate)
        {
            autoMineTimer = 0.0f;
            
            // Step along ray from Lock A toward Lock B looking for solid blocks (identical to auto-place)
            Vec3 direction = m_inputState.lockBPos - m_inputState.lockAPos;
            float maxDist = direction.length();
            direction = direction.normalized();
            
            Vec3 currentPos = m_inputState.lockAPos + Vec3(0.5f, 0.5f, 0.5f);
            const float stepSize = 0.5f;
            
            for (float dist = 0.0f; dist < maxDist; dist += stepSize)
            {
                Vec3 checkPos = currentPos + direction * dist;
                Vec3 blockPos(
                    std::floor(checkPos.x),
                    std::floor(checkPos.y),
                    std::floor(checkPos.z)
                );
                
                uint8_t blockType = m_clientWorld->getIslandSystem()->getVoxelFromIsland(
                    m_inputState.lockAIslandID, blockPos);
                
                if (blockType != 0)  // Found solid block
                {
                    const BlockTypeInfo* blockInfo = BlockTypeRegistry::getInstance().getBlockType(blockType);
                    uint8_t durability = blockInfo ? blockInfo->properties.durability : 3;
                    
                    bool shouldBreak = m_blockDamage.applyHit(m_inputState.lockAIslandID, blockPos, durability);
                    
                    if (shouldBreak)
                    {
                        m_clientWorld->applyPredictedVoxelChange(m_inputState.lockAIslandID, blockPos, 0, blockType);
                        
                        if (m_networkManager && m_networkManager->getClient() &&
                            m_networkManager->getClient()->isConnected())
                        {
                            uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(
                                m_inputState.lockAIslandID, blockPos, 0);
                            m_pendingVoxelChanges[seqNum] = {m_inputState.lockAIslandID, blockPos, 0, blockType};
                        }
                    }
                    break;  // Only mine one block per tick
                }
            }
        }
    }
    
    else
    {
        autoMineDebugOnce = false;
    }
    
    if (!leftClick)
    {
        lockACreated = false;
        autoMineTimer = 0.0f;  // Reset timer when button released
        autoMineDebugOnce = false;
    }
    
    // Handle LMB click to break block (only on release if it was a short click, not a hold)
    if (leftReleased && leftReleaseDuration < m_inputState.holdThreshold && m_inputState.cachedTargetBlock.hit)
    {
        uint32_t islandID = m_inputState.cachedTargetBlock.islandID;
        Vec3 blockPos = m_inputState.cachedTargetBlock.localBlockPos;
        
        // Get block type and durability
        uint8_t blockType = m_clientWorld->getIslandSystem()->getVoxelFromIsland(islandID, blockPos);
        
        if (blockType != 0)  // Not air
        {
            // Get block durability from registry
            const BlockTypeInfo* blockInfo = BlockTypeRegistry::getInstance().getBlockType(blockType);
            uint8_t durability = blockInfo ? blockInfo->properties.durability : 3;
            
            // Apply 1 hit of damage
            bool shouldBreak = m_blockDamage.applyHit(islandID, blockPos, durability);
            
            if (shouldBreak)
            {
                // Block breaks - apply change
                m_clientWorld->applyPredictedVoxelChange(islandID, blockPos, 0, blockType);
                
                // Send to server for validation
                if (m_networkManager && m_networkManager->getClient() &&
                    m_networkManager->getClient()->isConnected())
                {
                    uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(islandID, blockPos, 0);
                    m_pendingVoxelChanges[seqNum] = {islandID, blockPos, 0, blockType};
                }
                
                std::cout << "💥 Block broken after " << static_cast<int>(durability) << " hits" << std::endl;
            }
            else
            {
                // Block damaged but not broken
                uint8_t currentDamage = m_blockDamage.getDamage(islandID, blockPos);
                float damagePercent = m_blockDamage.getDamagePercent(islandID, blockPos, durability);
                std::cout << "🔨 Hit! Damage: " << static_cast<int>(currentDamage) << "/" 
                         << static_cast<int>(durability) << " (" 
                         << static_cast<int>(damagePercent * 100.0f) << "%)" << std::endl;
            }
        }
    }
    
    m_inputState.leftMouseWasPressed = leftClick;

    // ==========================================
    // RIGHT CLICK/HOLD - INSTANT PLACE OR LOCK B OR AUTO-PLACE
    // ==========================================
    // Click (< 80ms) = instant place block or recipe lock
    // Hold (>= 80ms) = create Lock B (geometry mode) OR auto-place if both locks exist
    
    // Handle RMB hold to create Lock B (only if Lock B doesn't exist)
    static bool lockBCreated = false;
    if (rightIsHold && !m_inputState.hasLockB && !lockBCreated && m_inputState.reticleHasTarget)
    {
        m_inputState.hasLockB = true;
        m_inputState.lockBPos = m_inputState.cachedTargetBlock.localBlockPos;
        m_inputState.lockBIslandID = m_inputState.cachedTargetBlock.islandID;
        lockBCreated = true;
        std::cout << "🔒 Lock B created at " << m_inputState.lockBPos.x << ", " 
                  << m_inputState.lockBPos.y << ", " << m_inputState.lockBPos.z << std::endl;
    }
    
    // Auto-place mode when both locks exist and RMB held (must have locked recipe)
    static float autoPlaceTimer = 0.0f;
    if (rightIsHold && m_inputState.hasLockA && m_inputState.hasLockB && 
        m_inputState.lockAIslandID == m_inputState.lockBIslandID && m_lockedRecipe)
    {
        autoPlaceTimer += deltaTime;
        
        if (autoPlaceTimer >= m_inputState.autoOperationRate)
        {
            autoPlaceTimer = 0.0f;
            
            // DDA raycast from Lock A toward Lock B to find empty spaces
            Vec3 direction = m_inputState.lockBPos - m_inputState.lockAPos;
            float maxDist = direction.length();
            direction = direction.normalized();
            
            // Step along ray looking for air blocks to fill
            Vec3 currentPos = m_inputState.lockAPos + Vec3(0.5f, 0.5f, 0.5f);
            const float stepSize = 0.5f;
            
            for (float dist = 0.0f; dist < maxDist; dist += stepSize)
            {
                Vec3 checkPos = currentPos + direction * dist;
                Vec3 blockPos(
                    std::floor(checkPos.x),
                    std::floor(checkPos.y),
                    std::floor(checkPos.z)
                );
                
                uint8_t blockType = m_clientWorld->getIslandSystem()->getVoxelFromIsland(
                    m_inputState.lockAIslandID, blockPos);
                
                if (blockType == 0)  // Found air block
                {
                    uint8_t blockToPlace = m_lockedRecipe->blockID;
                    
                    m_clientWorld->applyPredictedVoxelChange(
                        m_inputState.lockAIslandID,
                        blockPos,
                        blockToPlace,
                        0);
                    
                    if (m_networkManager && m_networkManager->getClient() &&
                        m_networkManager->getClient()->isConnected())
                    {
                        uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(
                            m_inputState.lockAIslandID, blockPos, blockToPlace);
                        m_pendingVoxelChanges[seqNum] = {
                            m_inputState.lockAIslandID,
                            blockPos,
                            blockToPlace,
                            0
                        };
                    }
                    break;  // Only place one block per tick
                }
            }
        }
    }
    
    if (!rightClick)
    {
        lockBCreated = false;
        autoPlaceTimer = 0.0f;  // Reset timer when button released
    }
    
    // Handle RMB click for instant placement or recipe locking (only on release if it was a short click, not a hold)
    if (rightReleased && rightReleaseDuration < m_inputState.holdThreshold)
    {
        // Priority 1: If we have a queued element sequence, try to lock/switch to it
        if (!m_elementQueue.isEmpty())
        {
            auto& recipeSystem = ElementRecipeSystem::getInstance();
            const BlockRecipe* newRecipe = recipeSystem.matchRecipe(m_elementQueue);
            
            if (newRecipe) {
                // Valid recipe - lock/switch to it
                m_lockedRecipe = newRecipe;
                std::cout << "🔒 Recipe locked: " << m_lockedRecipe->name 
                          << " (" << m_lockedRecipe->formula << ")" << std::endl;
                m_elementQueue.clear();
            } else {
                // Invalid recipe - clear the queue
                std::cout << "❌ No recipe matches " << m_elementQueue.toFormula() << " - clearing queue" << std::endl;
                m_elementQueue.clear();
            }
        }
        // If no queue but we have a locked recipe and valid reticle target, place block
        else if (m_lockedRecipe && m_inputState.reticleHasTarget)
        {
            Vec3 placePos = m_inputState.reticleSnapPos;
            uint32_t islandID = m_inputState.reticleIslandID;
            uint8_t existingVoxel = m_clientWorld->getVoxel(islandID, placePos);

            if (existingVoxel == 0)
            {
                // Use locked recipe block
                uint8_t blockToPlace = m_lockedRecipe->blockID;
                
                // OPTIMISTIC UPDATE: Apply predicted change immediately on client
                m_clientWorld->applyPredictedVoxelChange(
                    islandID,
                    placePos,
                    blockToPlace,
                    0);  // previousType is air
                
                // Send network request for server validation (sequence number is tracked internally)
                if (m_networkManager && m_networkManager->getClient() &&
                    m_networkManager->getClient()->isConnected())
                {
                    uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(
                        islandID, placePos, blockToPlace);
                    
                    // Track this prediction
                    m_pendingVoxelChanges[seqNum] = {
                        islandID,
                        placePos,
                        blockToPlace,  // predictedType
                        existingVoxel  // previousType (air)
                    };
                }

                // Server will confirm or revert via VoxelChangeUpdate
                
                // Keep recipe locked for continuous placement
                std::cout << "Block placed (" << m_lockedRecipe->name << " still locked)" << std::endl;
                m_inputState.raycastTimer = 0.0f;
                
                // Clear any damage on the placed position (in case we're overwriting damaged block)
                m_blockDamage.clearDamage(islandID, placePos);
            }
        }
    }
    
    // RMB Hold - Create Lock B
    static bool lockBTriggered = false;
    if (rightIsHold && !lockBTriggered && !m_inputState.hasLockB && m_inputState.reticleHasTarget)
    {
        // Create Lock B at current reticle position
        m_inputState.hasLockB = true;
        m_inputState.lockBPos = m_inputState.reticleWorldPos;
        m_inputState.lockBIslandID = m_inputState.reticleIslandID;
        lockBTriggered = true;
        std::cout << "🔒 Lock B created at (" << m_inputState.lockBPos.x << ", " 
                  << m_inputState.lockBPos.y << ", " << m_inputState.lockBPos.z << ")" << std::endl;
    }
    if (!rightClick)
    {
        lockBTriggered = false;
    }
    
    m_inputState.rightMouseWasPressed = rightClick;
}

void GameClient::renderWaitingScreen()
{
    // Simple waiting screen for remote clients
    // The gradient sky will be rendered by the deferred lighting shader automatically
}

void GameClient::renderUI()
{
    // Start ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    // Render HUD
    if (m_hud)
    {
        // Update HUD state
        m_hud->setPlayerPosition(m_playerController.getCamera().position.x, m_playerController.getCamera().position.y, m_playerController.getCamera().position.z);
        m_hud->setPlayerHealth(100.0f, 100.0f); // TODO: Wire up actual health system
        
        // Calculate FPS from delta time
        float fps = (m_lastFrameDeltaTime > 0.0001f) ? (1.0f / m_lastFrameDeltaTime) : 60.0f;
        m_hud->setFPS(fps);
        
        // Set current block in hand (TODO: get from inventory/hotbar)
        m_hud->setCurrentBlock("Stone");
        
        // Set target block (block player is looking at) with elemental formula
        if (m_inputState.cachedTargetBlock.hit && m_clientWorld)
        {
            uint8_t blockID = m_clientWorld->getVoxel(
                m_inputState.cachedTargetBlock.islandID,
                m_inputState.cachedTargetBlock.localBlockPos
            );
            
            auto& registry = BlockTypeRegistry::getInstance();
            const BlockTypeInfo* blockInfo = registry.getBlockType(blockID);
            
            // Try to find recipe for this block to show its formula
            std::string formula = "";
            auto& recipeSystem = ElementRecipeSystem::getInstance();
            for (const auto& recipe : recipeSystem.getAllRecipes()) {
                if (recipe.blockID == blockID) {
                    formula = recipe.formula;
                    break;
                }
            }
            
            if (blockInfo)
            {
                m_hud->setTargetBlock(blockInfo->name, formula);
            }
            else
            {
                m_hud->clearTargetBlock();
            }
        }
        else
        {
            m_hud->clearTargetBlock();
        }
        
        // Render HUD overlay
        m_hud->render(m_lastFrameDeltaTime);
        
        // NEW: Render element queue hotbar (with customizable elements)
        m_hud->renderElementQueue(m_elementQueue, m_lockedRecipe, m_hotbarElements);
        
        // Render periodic table UI if open
        if (m_periodicTableUI->isOpen()) {
            m_periodicTableUI->render(m_hotbarElements);
        }
    }
    
    // Show connection status for remote clients
    if (m_isRemoteClient)
    {
        // Additional remote client UI could go here
    }
    
    // Finalize ImGui frame
    ImGui::Render();
    // ImGui draw data will be rendered in renderVulkan()
}

void GameClient::onWindowResize(int width, int height)
{
    m_windowWidth = width;
    m_windowHeight = height;
    
    // Vulkan will handle swapchain recreation automatically
}

void GameClient::handleWorldStateReceived(const WorldStateMessage& worldState)
{
    // Create a new ClientWorld for the client based on server data
    m_clientWorld = new ClientWorld();

    if (!m_clientWorld->initialize(false))
    {  // Don't create default world, we'll use server data
        std::cerr << "Failed to initialize client game state!" << std::endl;
        delete m_clientWorld;
        m_clientWorld = nullptr;
        return;
    }

    // Connect physics system to CLIENT's island system for collision detection
    if (m_clientWorld && m_clientWorld->getIslandSystem())
    {
        m_clientPhysics.setIslandSystem(m_clientWorld->getIslandSystem());
        // Mark chunks as client-side (need GPU upload)
        m_clientWorld->getIslandSystem()->setIsClient(true);
    }

    // Spawn player at server-provided location
    m_playerController.setPosition(worldState.playerSpawnPosition);
}

void GameClient::registerChunkWithRenderer(VoxelChunk* chunk, FloatingIsland* island, uint32_t islandID, const Vec3& chunkCoord)
{
    // Register with Vulkan renderer
    if (chunk && island)
    {
        // Calculate chunk offset in world units
        glm::vec3 chunkOffset(chunkCoord.x * VoxelChunk::SIZE,
                              chunkCoord.y * VoxelChunk::SIZE,
                              chunkCoord.z * VoxelChunk::SIZE);
        
        if (m_vulkanQuadRenderer)
        {
            m_vulkanQuadRenderer->registerChunk(chunk, islandID, chunkOffset);
        }
        
        if (m_vulkanModelRenderer)
        {
            m_vulkanModelRenderer->updateChunkInstances(chunk, islandID, chunkOffset);
        }
        
        // Generate mesh async - uploads when ready
        chunk->generateMeshAsync();
        m_chunksWithPendingMeshes.push_back(chunk);
    }
}

void GameClient::syncPhysicsToChunks()
{
    if (!m_clientWorld)
        return;
    
    auto* islandSystem = m_clientWorld->getIslandSystem();
    if (!islandSystem)
        return;
    
    // Vulkan renderer updates transforms directly via UBOs
    // No per-chunk transform updates needed
}

void GameClient::handleCompressedIslandReceived(uint32_t islandID, const Vec3& position,
                                                const uint8_t* voxelData, uint32_t dataSize)
{
    (void)islandID; // Island ID tracked by IslandChunkSystem internally
    if (!m_clientWorld)
    {
        std::cerr << "Cannot handle island data: No game state initialized" << std::endl;
        return;
    }

    auto* islandSystem = m_clientWorld->getIslandSystem();
    if (!islandSystem)
    {
        std::cerr << "No island system available" << std::endl;
        return;
    }

    // Create the island at the specified position with the server's ID
    uint32_t localIslandID = islandSystem->createIsland(position);

    // Get the island and apply the voxel data
    FloatingIsland* island = islandSystem->getIsland(localIslandID);
    if (island)
    {
        // Create the main chunk if it doesn't exist (client islands don't auto-generate chunks)
        // For backward compatibility, use the origin chunk (0,0,0)
        Vec3 originChunk(0, 0, 0);
        if (island->chunks.find(originChunk) == island->chunks.end())
        {
            islandSystem->addChunkToIsland(localIslandID, originChunk);
        }

        VoxelChunk* chunk = islandSystem->getChunkFromIsland(localIslandID, originChunk);
        if (chunk)
        {
            // Apply the voxel data directly - this replaces any procedural generation
            chunk->setRawVoxelData(voxelData, dataSize);
            
            // Register chunk with renderer (will queue mesh generation)
            registerChunkWithRenderer(chunk, island, localIslandID, originChunk);
        }
        else
        {
            std::cerr << "Failed to create main chunk for island " << localIslandID << std::endl;
        }
    }
    else
    {
        std::cerr << "Failed to retrieve island with local ID: " << localIslandID << std::endl;
    }
}

void GameClient::handleCompressedChunkReceived(uint32_t islandID, const Vec3& chunkCoord, const Vec3& islandPosition,
                                               const uint8_t* voxelData, uint32_t dataSize)
{
    if (!m_clientWorld)
    {
        std::cerr << "Cannot handle chunk data: No game state initialized" << std::endl;
        return;
    }

    auto* islandSystem = m_clientWorld->getIslandSystem();
    if (!islandSystem)
    {
        std::cerr << "No island system available" << std::endl;
        return;
    }

    // Create or get the island
    FloatingIsland* island = islandSystem->getIsland(islandID);
    
    if (!island)
    {
        // Create the island with the server's ID to keep them in sync
        islandSystem->createIsland(islandPosition, islandID);
        island = islandSystem->getIsland(islandID);

        if (!island)
        {
            std::cerr << "Failed to create island " << islandID << std::endl;
            return;
        }
        
        std::cout << "📦 Created new island " << islandID << " from server" << std::endl;
        
        // Update Vulkan renderer with island transform
        if (m_vulkanQuadRenderer) {
            m_vulkanQuadRenderer->updateIslandTransform(islandID, island->getTransformMatrix());
            if (m_vulkanModelRenderer)
            {
                m_vulkanModelRenderer->updateIslandTransform(islandID, island->getTransformMatrix());
            }
        }
    }

    // Add chunk to island if it doesn't exist
    VoxelChunk* chunk = islandSystem->getChunkFromIsland(islandID, chunkCoord);
    if (!chunk)
    {
        islandSystem->addChunkToIsland(islandID, chunkCoord);
        chunk = islandSystem->getChunkFromIsland(islandID, chunkCoord);
    }

    if (chunk)
    {
        // Apply the voxel data directly
        chunk->setRawVoxelData(voxelData, dataSize);
        
        // Register chunk with Vulkan renderer
        glm::vec3 chunkOffset(chunkCoord.x * VoxelChunk::SIZE,
                              chunkCoord.y * VoxelChunk::SIZE,
                              chunkCoord.z * VoxelChunk::SIZE);
        
        if (m_vulkanQuadRenderer) {
            m_vulkanQuadRenderer->registerChunk(chunk, islandID, chunkOffset);
        }
        
        if (m_vulkanModelRenderer) {
            m_vulkanModelRenderer->updateChunkInstances(chunk, islandID, chunkOffset);
        }

        // Generate mesh async - uploads when ready
        chunk->generateMeshAsync();
        m_chunksWithPendingMeshes.push_back(chunk);
    }
    else
    {
        std::cerr << "Failed to create chunk " << chunkCoord.x << "," << chunkCoord.y << "," << chunkCoord.z
                  << " for island " << islandID << std::endl;
    }
}

void GameClient::handleVoxelChangeReceived(const VoxelChangeUpdate& update)
{
    if (!m_clientWorld)
    {
        std::cerr << "Cannot apply voxel change: no game state!" << std::endl;
        return;
    }

    // Check if this is a confirmation of our own prediction
    auto it = m_pendingVoxelChanges.find(update.sequenceNumber);
    if (it != m_pendingVoxelChanges.end())
    {
        const PendingVoxelChange& prediction = it->second;
        
        // Check if server's result matches our prediction
        if (prediction.islandID == update.islandID &&
            prediction.localPos == update.localPos &&
            prediction.predictedType == update.voxelType)
        {
            // Server confirmed our prediction - reconcile (no-op if already applied)
            std::cout << "[CLIENT] Server confirmed prediction (seq " << update.sequenceNumber << ")" << std::endl;
            m_clientWorld->reconcileVoxelChange(
                update.sequenceNumber, update.islandID, update.localPos, update.voxelType);
        }
        else
        {
            // Server rejected or modified our prediction - apply correction
            std::cout << "[CLIENT] Server corrected prediction (seq " << update.sequenceNumber 
                      << ") - applying server's version" << std::endl;
            m_clientWorld->reconcileVoxelChange(
                update.sequenceNumber, update.islandID, update.localPos, update.voxelType);
        }
        
        // Remove from pending predictions
        m_pendingVoxelChanges.erase(it);
    }
    else
    {
        // This is a change from another player or server-initiated - apply directly
        m_clientWorld->getIslandSystem()->setVoxelWithMesh(
            update.islandID, update.localPos, update.voxelType);
    }
    
    // **FIXED**: Always force immediate raycast update when server sends voxel changes
    // This ensures block selection is immediately accurate after server updates
    m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
        m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_clientWorld->getIslandSystem());
    m_inputState.raycastTimer = 0.0f;
}

void GameClient::handleEntityStateUpdate(const EntityStateUpdate& update)
{
    if (!m_clientWorld)
    {
        return;
    }

    // Handle different entity types
    switch (update.entityType)
    {
        case 1:
        {  // Island
            auto* islandSystem = m_clientWorld->getIslandSystem();
            if (islandSystem)
            {
                FloatingIsland* island = islandSystem->getIsland(update.entityID);
                if (island)
                {
                    // Apply server-authoritative velocity for client-side physics simulation
                    // This allows smooth movement while maintaining server authority

                    Vec3 currentPos = island->physicsCenter;
                    Vec3 serverPos = update.position;
                    Vec3 positionError = serverPos - currentPos;

                    // Calculate position error magnitude
                    float errorMagnitude = sqrtf(positionError.x * positionError.x +
                                                 positionError.y * positionError.y +
                                                 positionError.z * positionError.z);

                    // Set velocity from server for physics simulation
                    island->velocity = update.velocity;
                    island->acceleration = update.acceleration;
                    
                    // Set rotation from server (server-authoritative)
                    island->rotation = update.rotation;
                    island->angularVelocity = update.angularVelocity;

                    // Apply position correction based on error magnitude
                    if (errorMagnitude > 2.0f)
                    {
                        // Large error: snap to server position (teleport/respawn case)
                        island->physicsCenter = serverPos;
                    }
                    else if (errorMagnitude > 0.1f)
                    {
                        // Small to medium error: apply gradual correction
                        // Add a correction velocity component to smoothly move toward server
                        // position
                        Vec3 correctionVelocity = positionError * 0.8f;  // Correction strength
                        island->velocity = island->velocity + correctionVelocity;
                    }
                    // If error is very small (< 0.1f), just use server velocity as-is

                    // Mark for physics update synchronization
                    island->needsPhysicsUpdate = true;
                    island->invalidateTransform();
                }
            }
            break;
        }
        case 3:
        {  // Fluid Particle
            // Find or create the fluid particle entity on client
            EntityID clientEntity = static_cast<EntityID>(update.entityID);
            
            // Check if entity exists
            auto* fluidComp = g_ecs.getComponent<FluidParticleComponent>(clientEntity);
            auto* transform = g_ecs.getComponent<TransformComponent>(clientEntity);
            
            if (!fluidComp || !transform) {
                // Entity doesn't exist on client - create it
                clientEntity = g_ecs.createEntity();
                
                TransformComponent newTransform;
                newTransform.position = update.position;
                g_ecs.addComponent(clientEntity, newTransform);
                
                FluidParticleComponent newFluid;
                newFluid.velocity = update.velocity;
                newFluid.state = FluidState::ACTIVE;
                g_ecs.addComponent(clientEntity, newFluid);
            } else {
                // Entity exists - update from server
                transform->position = update.position;
                fluidComp->velocity = update.velocity;
            }
            break;
        }
        case 0:  // Player (future implementation)
        case 2:  // NPC (future implementation)
        default:
            // Handle other entity types in the future
            break;
    }
}

void GameClient::handleFluidParticleSpawn(const FluidParticleSpawnMessage& msg)
{
    extern ECSWorld g_ecs;
    
    std::cout << "[CLIENT] Spawning fluid particle entity " << msg.entityID 
              << " at (" << msg.worldPosition.x << ", " << msg.worldPosition.y << ", " << msg.worldPosition.z << ")" << std::endl;
    
    // Create entity with specific ID (client mirrors server's entity ID)
    EntityID entity = g_ecs.createEntityWithID(msg.entityID);
    
    // Add transform component
    TransformComponent transform;
    transform.position = msg.worldPosition;
    g_ecs.addComponent(entity, transform);
    
    // Add fluid component (client-side, render-only)
    FluidParticleComponent fluidComp;
    fluidComp.state = FluidState::ACTIVE;
    fluidComp.velocity = msg.velocity;
    fluidComp.sourceIslandID = msg.islandID;
    fluidComp.originalVoxelPos = msg.originalVoxelPos;
    g_ecs.addComponent(entity, fluidComp);
}

void GameClient::handleFluidParticleDespawn(const FluidParticleDespawnMessage& msg)
{
    extern ECSWorld g_ecs;
    
    std::cout << "[CLIENT] Despawning fluid particle entity " << msg.entityID << std::endl;
    
    // If particle settled and should create voxel
    if (msg.shouldCreateVoxel && m_clientWorld)
    {
        std::cout << "[CLIENT] Placing water voxel at (" << msg.settledVoxelPos.x 
                  << ", " << msg.settledVoxelPos.y << ", " << msg.settledVoxelPos.z << ")" << std::endl;
        
        // Place water voxel on client
        m_clientWorld->applyServerVoxelChange(msg.islandID, msg.settledVoxelPos, BlockID::WATER);
    }
    
    // Destroy the particle entity
    g_ecs.destroyEntity(msg.entityID);
}
