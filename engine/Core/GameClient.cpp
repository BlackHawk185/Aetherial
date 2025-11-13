// GameClient.cpp - Client-side rendering and input implementation
#include "GameClient.h"

#include <GLFW/glfw3.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

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
#include "../Rendering/BlockHighlightRenderer.h"

#include "../Rendering/GBuffer.h"
#include "../Rendering/DeferredLightingPass.h"
#include "../Rendering/PostProcessingPipeline.h"
#include "../Rendering/HDRFramebuffer.h"
#include "../Rendering/SkyRenderer.h"
#include "../Rendering/VolumetricCloudRenderer.h"
#include "../UI/HUD.h"
#include "../UI/PeriodicTableUI.h"  // NEW: Periodic table UI for hotbar binding
#include "../Core/Window.h"
#include "../Rendering/InstancedQuadRenderer.h"
#include "../Rendering/ModelInstanceRenderer.h"
#include "../Rendering/TextureManager.h"
#include "../Rendering/CascadedShadowMap.h"
#include "../Rendering/GPUMeshQueue.h"  // Main-thread mesh generation queue
#include "../Physics/PhysicsSystem.h"  // For ground detection
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "../Time/TimeEffects.h"
#include "../Time/DayNightController.h"
#include "../World/VoxelChunk.h"  // For accessing voxel data

// External systems
extern TimeEffects* g_timeEffects;
extern LightMap g_lightMap;

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

    if (!initializeGraphics())
    {
        return false;
    }

    // Initialize greedy mesh queue (main-thread mesh generation)
    if (!g_greedyMeshQueue)
    {
        g_greedyMeshQueue = std::make_unique<GreedyMeshQueue>();
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

    // Process mesh generation queue (processes up to 128 chunks per frame for faster updates)
    if (g_greedyMeshQueue)
    {
        g_greedyMeshQueue->processQueue(128);
    } else {
        static bool warnedOnce = false;
        if (!warnedOnce) {
            std::cout << "[GAME CLIENT] WARNING: No mesh queue available in update loop!" << std::endl;
            warnedOnce = true;
        }
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

    // Update model instancing time (wind animation)
    if (g_modelRenderer)
    {
        g_modelRenderer->update(deltaTime);
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

    // Shutdown GPU mesh queue
    if (g_greedyMeshQueue)
    {
        g_greedyMeshQueue.reset();
    }

    // Disconnect from game state
    m_clientWorld = nullptr;

    // Cleanup renderers
    if (g_instancedQuadRenderer)
    {
        g_instancedQuadRenderer->shutdown();
        g_instancedQuadRenderer.reset();
        std::cout << "InstancedQuadRenderer shutdown" << std::endl;
    }

    if (g_modelRenderer)
    {
        g_modelRenderer->shutdown();
        g_modelRenderer.reset();
    }
    
    // Cleanup ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

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
    
    // Clear depth buffer only (gradient sky will be rendered by deferred lighting shader)
    glClear(GL_DEPTH_BUFFER_BIT);

    // Render world (only if we have local game state)
    if (m_clientWorld)
    {
        PROFILE_SCOPE("renderWorld");
        renderWorld();
    }
    else if (m_isRemoteClient)
    {
        // Render waiting screen for remote clients
        PROFILE_SCOPE("renderWaitingScreen");
        renderWaitingScreen();
    }

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

    // Set up mouse capture on underlying GLFW window
    glfwSetInputMode(m_window->getHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    return true;
}

bool GameClient::initializeGraphics()
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
    
    // Initialize MDI quad renderer (greedy meshing + multi-draw indirect)
    g_instancedQuadRenderer = std::make_unique<InstancedQuadRenderer>();
    if (!g_instancedQuadRenderer->initialize())
    {
        std::cerr << "❌ Failed to initialize InstancedQuadRenderer!" << std::endl;
        g_instancedQuadRenderer.reset();
        return false;
    }
    std::cout << "✅ InstancedQuadRenderer initialized - MDI rendering ready!" << std::endl;

    // Initialize light map system (must happen before renderers that use it)
    // 4 cascades: 2 for sun (near+far), 2 for moon (near+far)
    if (!g_lightMap.initialize(8192, 4))
    {
        std::cerr << "❌ Failed to initialize light map system!" << std::endl;
        return false;
    }

    // Initialize G-buffer for deferred rendering
    if (!g_gBuffer.initialize(m_windowWidth, m_windowHeight))
    {
        std::cerr << "❌ Failed to initialize G-buffer!" << std::endl;
        return false;
    }

    // Initialize deferred lighting pass
    if (!g_deferredLighting.initialize())
    {
        std::cerr << "❌ Failed to initialize deferred lighting pass!" << std::endl;
        return false;
    }

    // Initialize HDR framebuffer for lighting output
    if (!g_hdrFramebuffer.initialize(m_windowWidth, m_windowHeight))
    {
        std::cerr << "❌ Failed to initialize HDR framebuffer!" << std::endl;
        return false;
    }

    // Initialize sky renderer
    if (!g_skyRenderer.initialize())
    {
        std::cerr << "❌ Failed to initialize sky renderer!" << std::endl;
        return false;
    }

    // Initialize volumetric cloud renderer
    if (!g_cloudRenderer.initialize())
    {
        std::cerr << "❌ Failed to initialize cloud renderer!" << std::endl;
        return false;
    }

    // Initialize post-processing pipeline (tone mapping only)
    if (!g_postProcessing.initialize(m_windowWidth, m_windowHeight))
    {
        std::cerr << "❌ Failed to initialize post-processing pipeline!" << std::endl;
        return false;
    }

    // Initialize model instancing renderer (decorative GLB like grass)
    g_modelRenderer = std::make_unique<ModelInstanceRenderer>();
    if (!g_modelRenderer->initialize())
    {
        std::cerr << "Failed to initialize ModelInstanceRenderer!" << std::endl;
        g_modelRenderer.reset();
        return false;
    }
    
    // Load all OBJ-type block models from registry
    auto& registry = BlockTypeRegistry::getInstance();
    for (const auto& blockType : registry.getAllBlockTypes())
    {
        if (blockType.renderType == BlockRenderType::OBJ && !blockType.assetPath.empty())
        {
            if (!g_modelRenderer->loadModel(blockType.id, blockType.assetPath))
            {
                std::cerr << "Warning: Failed to load model for '" << blockType.name 
                          << "' from " << blockType.assetPath << std::endl;
            }
        }
    }

    // Initialize block highlighter for selected block wireframe
    m_blockHighlighter = std::make_unique<BlockHighlightRenderer>();
    if (!m_blockHighlighter->initialize())
    {
        std::cerr << "Warning: Failed to initialize BlockHighlightRenderer" << std::endl;
        m_blockHighlighter.reset();
    }
    
    // Initialize HUD overlay
    m_hud = std::make_unique<HUD>();
    
    // Initialize Periodic Table UI
    m_periodicTableUI = std::make_unique<PeriodicTableUI>();

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    ImGui::StyleColorsDark();
    
    ImGui_ImplGlfw_InitForOpenGL(m_window->getHandle(), true);
    ImGui_ImplOpenGL3_Init("#version 460");

    return true;
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
    // Toggle post-processing (press P)
    static bool wasPostProcessingKeyPressed = false;
    bool isPostProcessingKeyPressed = glfwGetKey(m_window->getHandle(), GLFW_KEY_P) == GLFW_PRESS;
    
    if (isPostProcessingKeyPressed && !wasPostProcessingKeyPressed)
    {
        g_postProcessing.setEnabled(!g_postProcessing.isEnabled());
        std::cout << (g_postProcessing.isEnabled() ? "🌈 Post-processing enabled (tone mapping)" : "🌈 Post-processing disabled (raw HDR)") << std::endl;
    }
    wasPostProcessingKeyPressed = isPostProcessingKeyPressed;

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

    // Update raycast timer for performance
    m_inputState.raycastTimer += deltaTime;
    if (m_inputState.raycastTimer > 0.05f)
    {  // 20 FPS raycasting for more responsive block selection
        m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
            m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_clientWorld->getIslandSystem());
        m_inputState.raycastTimer = 0.0f;
    }

    bool leftClick = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool rightClick = glfwGetMouseButton(m_window->getHandle(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    // Left click - break block
    if (leftClick && !m_inputState.leftMousePressed)
    {
        m_inputState.leftMousePressed = true;

        if (m_inputState.cachedTargetBlock.hit)
        {
            // Get the current voxel type before changing it
            uint8_t previousType = m_clientWorld->getIslandSystem()->getVoxelFromIsland(
                m_inputState.cachedTargetBlock.islandID,
                m_inputState.cachedTargetBlock.localBlockPos);
            
            // OPTIMISTIC UPDATE: Apply predicted change immediately on client
            m_clientWorld->applyPredictedVoxelChange(
                m_inputState.cachedTargetBlock.islandID,
                m_inputState.cachedTargetBlock.localBlockPos,
                0,  // newType (air = break block)
                previousType);
            
            // Send network request for server validation (sequence number is tracked internally)
            if (m_networkManager && m_networkManager->getClient() &&
                m_networkManager->getClient()->isConnected())
            {
                uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(
                    m_inputState.cachedTargetBlock.islandID,
                    m_inputState.cachedTargetBlock.localBlockPos, 0);
                
                // Track this prediction for reconciliation
                m_pendingVoxelChanges[seqNum] = {
                    m_inputState.cachedTargetBlock.islandID,
                    m_inputState.cachedTargetBlock.localBlockPos,
                    0,  // predictedType (air)
                    previousType  // For rollback if server rejects
                };
            }

            // Server will confirm or revert via VoxelChangeUpdate

            // Clear the cached target block immediately to remove the yellow outline
            m_inputState.cachedTargetBlock = RayHit();

            // **FIXED**: Force immediate raycast to update block selection after breaking
            m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
                m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_clientWorld->getIslandSystem());
            m_inputState.raycastTimer = 0.0f;
        }
    }
    else if (!leftClick)
    {
        m_inputState.leftMousePressed = false;
    }

    // Right click - place block or lock/switch recipe
    if (rightClick && !m_inputState.rightMousePressed)
    {
        m_inputState.rightMousePressed = true;

        // If we have a queued element sequence, try to lock/switch to it
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
        // If no queue but we have a locked recipe and valid target, place block
        else if (m_lockedRecipe && m_inputState.cachedTargetBlock.hit)
        {
            Vec3 placePos = VoxelRaycaster::getPlacementPosition(m_inputState.cachedTargetBlock);
            uint8_t existingVoxel =
                m_clientWorld->getVoxel(m_inputState.cachedTargetBlock.islandID, placePos);

            if (existingVoxel == 0)
            {
                // Use locked recipe block
                uint8_t blockToPlace = m_lockedRecipe->blockID;
                
                // OPTIMISTIC UPDATE: Apply predicted change immediately on client
                m_clientWorld->applyPredictedVoxelChange(
                    m_inputState.cachedTargetBlock.islandID,
                    placePos,
                    blockToPlace,
                    0);  // previousType is air
                
                // Send network request for server validation (sequence number is tracked internally)
                if (m_networkManager && m_networkManager->getClient() &&
                    m_networkManager->getClient()->isConnected())
                {
                    uint32_t seqNum = m_networkManager->getClient()->sendVoxelChangeRequest(
                        m_inputState.cachedTargetBlock.islandID, placePos, blockToPlace);
                    
                    // Track this prediction
                    m_pendingVoxelChanges[seqNum] = {
                        m_inputState.cachedTargetBlock.islandID,
                        placePos,
                        blockToPlace,  // predictedType
                        existingVoxel  // previousType (air)
                    };
                }

                // Server will confirm or revert via VoxelChangeUpdate
                
                // Keep recipe locked for continuous placement
                std::cout << "Block placed (" << m_lockedRecipe->name << " still locked)" << std::endl;

                // Clear the cached target block to refresh the selection
                m_inputState.cachedTargetBlock = RayHit();

                // **FIXED**: Force immediate raycast to update block selection after placing
                m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
                    m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_clientWorld->getIslandSystem());
                m_inputState.raycastTimer = 0.0f;
            }
        }
    }
    else if (!rightClick)
    {
        m_inputState.rightMousePressed = false;
    }
}

void GameClient::renderWorld()
{
    PROFILE_SCOPE("GameClient::renderWorld");
    
    if (!m_clientWorld)
    {
        return;
    }
    
    // Sync island physics to chunk transforms (updates GLB instances)
    {
        PROFILE_SCOPE("syncPhysicsToChunks");
        syncPhysicsToChunks();
    }

    // Get camera matrices once
    float aspect = (float)m_windowWidth / (float)m_windowHeight;
    glm::mat4 projectionMatrix = m_playerController.getCamera().getProjectionMatrix(aspect);
    glm::mat4 viewMatrix = m_playerController.getCamera().getViewMatrix();
    
    // Update and get frustum for culling
    m_playerController.getCamera().updateFrustum(aspect);
    const Frustum& frustum = m_playerController.getCamera().getFrustum();
    
    // Get visible chunks using frustum culling
    std::vector<VoxelChunk*> visibleChunks;
    {
        PROFILE_SCOPE("FrustumCull");
        m_clientWorld->getIslandSystem()->getVisibleChunksFrustum(frustum, visibleChunks);
    }

    // === DEFERRED RENDERING PIPELINE ===
    
    // 1. G-Buffer Pass: Render scene geometry to G-buffer (albedo, normal, position, metadata)
    {
        PROFILE_SCOPE("GBuffer_Pass");
        
        g_gBuffer.bindForGeometryPass();
        
        glm::mat4 viewProjection = projectionMatrix * viewMatrix;
        
        if (g_instancedQuadRenderer)
        {
            g_instancedQuadRenderer->renderToGBufferCulledMDI(viewProjection, viewMatrix, visibleChunks);
        }
        
        // Render GLB models to G-buffer (frustum culled)
        if (g_modelRenderer)
        {
            g_modelRenderer->renderToGBufferVisible(viewMatrix, projectionMatrix, visibleChunks);
        }
        
        g_gBuffer.unbind();
    }
    
    // 2. Light Depth Pass: Render shadow maps (uses GBuffer for occlusion culling)
    // Throttled - only update every Nth frame for performance
    m_frameCounter++;
    if (m_frameCounter % m_shadowUpdateInterval == 0)
    {
        renderLightDepthPass(visibleChunks);
    }
    
    // Get shared data for lighting and sky rendering
    Vec3 sunDir = m_dayNightController ? m_dayNightController->getSunDirection() : Vec3(-0.3f, -1.0f, -0.2f).normalized();
    Vec3 moonDir = m_dayNightController ? m_dayNightController->getMoonDirection() : Vec3(0.3f, -1.0f, 0.2f).normalized();
    glm::vec3 sunDirGLM(sunDir.x, sunDir.y, sunDir.z);
    glm::vec3 moonDirGLM(moonDir.x, moonDir.y, moonDir.z);
    
    float sunIntensity = m_dayNightController ? m_dayNightController->getSunIntensity() : 0.8f;
    float moonIntensity = m_dayNightController ? m_dayNightController->getMoonIntensity() : 0.15f;
    
    const Vec3& camPos = m_playerController.getCamera().position;
    glm::vec3 cameraPosGLM(camPos.x, camPos.y, camPos.z);

    // 3. Lighting Pass: Read G-buffer, apply light maps, output to HDR framebuffer
    {
        PROFILE_SCOPE("Deferred_Lighting_Pass");
        
        // Update cascade data in deferred lighting pass (4 cascades: 2 sun + 2 moon)
        for (int i = 0; i < g_lightMap.getNumCascades(); ++i)
        {
            const CascadeData& cascade = g_lightMap.getCascade(i);
            g_deferredLighting.setCascadeData(i, cascade.viewProj, cascade.splitDistance, cascade.orthoSize);
        }
        
        // Render full-screen quad with deferred lighting to HDR framebuffer
        float timeOfDay = m_dayNightController ? m_dayNightController->getTimeOfDay() : 12.0f;
        g_deferredLighting.render(sunDirGLM, moonDirGLM, sunIntensity, moonIntensity, cameraPosGLM, timeOfDay);
    }
    
    // 3. Sky Pass: Render sky gradient with sun disc to HDR framebuffer
    {
        PROFILE_SCOPE("Sky_Pass");
        
        // Bind HDR framebuffer and copy depth from G-buffer
        g_hdrFramebuffer.bind();
        
        // Copy depth from G-buffer to HDR framebuffer for proper depth testing
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gBuffer.getFBO());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_hdrFramebuffer.getFBO());
        glBlitFramebuffer(0, 0, m_windowWidth, m_windowHeight, 0, 0, m_windowWidth, m_windowHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        
        // Render sky (will only render where depth = 1.0, i.e., background pixels)
        float timeOfDay = m_dayNightController ? m_dayNightController->getTimeOfDay() : 12.0f;
        g_skyRenderer.render(sunDirGLM, sunIntensity, moonDirGLM, moonIntensity, 
                           cameraPosGLM, viewMatrix, projectionMatrix, timeOfDay);
        
        // Render volumetric clouds (after sky, before transparent objects)
        g_cloudRenderer.render(sunDirGLM, sunIntensity, cameraPosGLM, 
                              viewMatrix, projectionMatrix, 
                              g_gBuffer.getDepthTexture(), timeOfDay);
        
        g_hdrFramebuffer.unbind();
    }
    
    // 3.5. Transparent Water Pass: Render water with alpha blending after lighting
    {
        PROFILE_SCOPE("Transparent_Water_Pass");
        
        // Bind HDR framebuffer (already has depth from G-buffer)
        g_hdrFramebuffer.bind();
        
        // Enable blending for transparency
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);     // Read depth buffer
        glDepthMask(GL_FALSE);       // Don't write to depth buffer
        
        // Render water blocks with transparency and SSR
        if (g_modelRenderer) {
            g_modelRenderer->renderWaterTransparent(viewMatrix, projectionMatrix, 
                                                   sunDirGLM, sunIntensity, 
                                                   moonDirGLM, moonIntensity, 
                                                   cameraPosGLM,
                                                   g_gBuffer.getPositionTexture(),
                                                   g_gBuffer.getNormalTexture(),
                                                   g_gBuffer.getAlbedoTexture(),
                                                   g_hdrFramebuffer.getColorTexture());
        }
        
        // Restore state
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        
        g_hdrFramebuffer.unbind();
    }
    
    // 4. Post-Processing Pass: Apply tone mapping, etc.
    {
        PROFILE_SCOPE("Post_Processing_Pass");
        
        // Get current framebuffer output
        GLuint currentTexture = g_hdrFramebuffer.getColorTexture();
        glm::mat4 viewProjectionMatrix = projectionMatrix * viewMatrix;
        
        // Apply post-processing effects (tone mapping only - godrays removed)
        g_postProcessing.process(currentTexture, g_gBuffer.getDepthTexture(), 
                               sunDirGLM, cameraPosGLM, viewProjectionMatrix);
    }
    
    // 5. Forward Pass: Render transparent/special objects (water, block highlight, UI)
    {
        PROFILE_SCOPE("Forward_Pass");
        
        // Render screen-space fluid (water GLBs + particles with smoothing)
        // Render block highlight (yellow wireframe cube on selected block)
        if (m_blockHighlighter && m_inputState.cachedTargetBlock.hit)
        {
            PROFILE_SCOPE("renderBlockHighlight");
            
            auto& islands = m_clientWorld->getIslandSystem()->getIslands();
            auto it = islands.find(m_inputState.cachedTargetBlock.islandID);
            if (it != islands.end())
            {
                const FloatingIsland& island = it->second;
                Vec3 localBlockPos = m_inputState.cachedTargetBlock.localBlockPos;
                glm::mat4 islandTransform = island.getTransformMatrix();
                
                m_blockHighlighter->render(localBlockPos, glm::value_ptr(islandTransform), 
                    glm::value_ptr(viewMatrix), glm::value_ptr(projectionMatrix));
            }
        }
    }
}

void GameClient::renderLightDepthPass(const std::vector<VoxelChunk*>& visibleChunks)
{
    PROFILE_SCOPE("GameClient::renderLightDepthPass");
    
    // Get camera matrices for GBuffer culling
    float aspect = (float)m_windowWidth / (float)m_windowHeight;
    glm::mat4 projectionMatrix = m_playerController.getCamera().getProjectionMatrix(aspect);
    glm::mat4 viewMatrix = m_playerController.getCamera().getViewMatrix();
    glm::mat4 viewProj = projectionMatrix * viewMatrix;
    
    // Get sun and moon directions from DayNightController
    Vec3 sunDir = m_dayNightController ? m_dayNightController->getSunDirection() : Vec3(-0.3f, -1.0f, -0.2f).normalized();
    Vec3 moonDir = m_dayNightController ? m_dayNightController->getMoonDirection() : Vec3(0.3f, -1.0f, 0.2f).normalized();
    glm::vec3 camPos(m_playerController.getCamera().position.x, m_playerController.getCamera().position.y, m_playerController.getCamera().position.z);
    
    int numCascades = g_lightMap.getNumCascades();
    
    // Cascade configuration
    const float cascade0Split = 128.0f;   // Near cascade max distance
    const float cascade1Split = 1000.0f;  // Far cascade = camera far plane
    const float nearOrthoSize = 64.0f;    // Near: 128x128 units coverage
    const float farOrthoSize = 1024.0f;   // Far: 2048x2048 units coverage
    
    // Render all 4 cascades (0-1: sun, 2-3: moon)
    for (int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx)
    {
        // Determine which light source (sun or moon) this cascade is for
        bool isSunCascade = (cascadeIdx < 2);
        glm::vec3 lightDir = isSunCascade ? glm::vec3(sunDir.x, sunDir.y, sunDir.z) : glm::vec3(moonDir.x, moonDir.y, moonDir.z);
        
        // Determine near or far within the light source pair
        bool isNear = (cascadeIdx % 2 == 0);
        float splitDistance = isNear ? cascade0Split : cascade1Split;
        float orthoSize = isNear ? nearOrthoSize : farOrthoSize;
        
        // Depth range must cover ALL shadow casters visible from camera
        // At sunset/sunrise (horizontal sun), shadow casters can be very far along light direction
        // Use a very large depth range to ensure we capture everything
        float depthRange = (orthoSize + splitDistance) * 4.0f;  // 4x safety margin for low sun angles
        float nearPlane = 0.1f;
        float farPlane = depthRange;
        
        // Build light view matrix centered on camera
        // Position light far back along light direction to capture shadow casters behind camera
        glm::vec3 lightTarget = camPos;
        glm::vec3 lightPos = camPos - lightDir * (depthRange * 0.5f);
        glm::mat4 lightView = glm::lookAt(lightPos, lightTarget, glm::vec3(0,1,0));
        
        // Build light projection with texel snapping for stability
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);
        
        // Snap to texel grid to prevent light map shimmering
        glm::vec4 centerLS = lightView * glm::vec4(lightTarget, 1.0f);
        int smWidth = g_lightMap.getSize();
        float texelSize = (2.0f * farOrthoSize) / float(smWidth);
        glm::vec2 snapped = glm::floor(glm::vec2(centerLS.x, centerLS.y) / texelSize) * texelSize;
        glm::vec2 delta = snapped - glm::vec2(centerLS.x, centerLS.y);
        glm::mat4 snapMat = glm::translate(glm::mat4(1.0f), glm::vec3(-delta.x, -delta.y, 0.0f));
        glm::mat4 lightVP = lightProj * snapMat * lightView;
        
        // Store cascade data for shader
        CascadeData cascadeData;
        cascadeData.viewProj = lightVP;
        cascadeData.splitDistance = splitDistance;
        cascadeData.orthoSize = orthoSize;
        g_lightMap.setCascadeData(cascadeIdx, cascadeData);
        
        // Render light depth pass for this cascade
        if (m_windowWidth > 0 && m_windowHeight > 0)
        {
            g_lightMap.bindForRendering(cascadeIdx);
            
            if (g_instancedQuadRenderer)
            {
                g_instancedQuadRenderer->renderLightDepthMDI(lightVP, visibleChunks,
                                                            g_gBuffer.getPositionTexture(), viewProj);
            }
            
            if (g_modelRenderer)
            {
                // GBuffer occlusion cull to only render camera-visible models
                glm::vec3 cameraPosGLM(m_playerController.getCamera().position.x,
                                      m_playerController.getCamera().position.y,
                                      m_playerController.getCamera().position.z);
                g_modelRenderer->renderLightDepthMDI(lightVP, visibleChunks, 
                                                    g_gBuffer.getPositionTexture(), viewProj, cameraPosGLM);
            }
            
            g_lightMap.unbindAfterRendering(m_windowWidth, m_windowHeight);
        }
    }
    
    // Restore culling for forward rendering pass
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    // Set lighting data for forward pass (use sun cascade 0 for basic forward lighting)
    glm::vec3 sunDirVec(sunDir.x, sunDir.y, sunDir.z);
    if (g_modelRenderer)
    {
        g_modelRenderer->setLightingData(g_lightMap.getCascade(0).viewProj, sunDirVec);
    }
}

void GameClient::renderWaitingScreen()
{
    // Simple waiting screen for remote clients
    // The gradient sky will be rendered by the deferred lighting shader automatically
}

void GameClient::renderUI()
{
    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
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
    
    // Finalize ImGui frame and render
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GameClient::onWindowResize(int width, int height)
{
    m_windowWidth = width;
    m_windowHeight = height;
    glViewport(0, 0, width, height);
    
    // Resize G-buffer to match new window size
    g_gBuffer.resize(width, height);
    
    // Resize HDR framebuffer to match new window size
    g_hdrFramebuffer.resize(width, height);
    
    // Resize post-processing pipeline to match new window size
    g_postProcessing.resize(width, height);
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

void GameClient::registerChunkWithRenderer(VoxelChunk* chunk, FloatingIsland* island, const Vec3& chunkCoord)
{
    if (!g_instancedQuadRenderer || !chunk || !island)
        return;
    
    glm::mat4 chunkTransform = island->getChunkTransform(chunkCoord);
    g_instancedQuadRenderer->registerChunk(chunk, chunkTransform);
}

void GameClient::syncPhysicsToChunks()
{
    if (!m_clientWorld || !g_instancedQuadRenderer)
        return;
    
    auto* islandSystem = m_clientWorld->getIslandSystem();
    if (!islandSystem)
        return;
    
    // Cache OBJ block types once instead of querying every iteration
    static std::vector<uint8_t> objBlockTypes;
    static bool objBlockTypesCached = false;
    if (!objBlockTypesCached)
    {
        auto& registry = BlockTypeRegistry::getInstance();
        for (const auto& blockType : registry.getAllBlockTypes())
        {
            if (blockType.renderType == BlockRenderType::OBJ)
            {
                objBlockTypes.push_back(blockType.id);
            }
        }
        objBlockTypesCached = true;
    }
    
    // Update transforms for islands that have moved
    for (auto& [id, island] : islandSystem->getIslands())
    {
        // Skip islands that haven't moved (need const_cast because getIslands() returns const ref)
        if (!island.needsPhysicsUpdate) continue;
        
        // Update transforms for all chunks in this island
        for (auto& [chunkCoord, chunk] : island.chunks)
        {
            if (!chunk) continue;
            
            // Use helper to compute chunk transform
            glm::mat4 chunkTransform = island.getChunkTransform(chunkCoord);
            
            // === UPDATE CHUNK QUAD RENDERER (voxel chunks) ===
            g_instancedQuadRenderer->updateChunkTransform(chunk.get(), chunkTransform);
            
            // === UPDATE GLB MODEL RENDERER (only for chunks with OBJ instances) ===
            if (g_modelRenderer && !objBlockTypes.empty())
            {
                for (uint8_t blockID : objBlockTypes)
                {
                    // OPTIMIZATION: Skip chunks with zero instances of this block type
                    const auto& instances = chunk->getModelInstances(blockID);
                    if (!instances.empty())
                    {
                        g_modelRenderer->updateModelMatrix(blockID, chunk.get(), chunkTransform);
                    }
                }
            }
        }
        
        // Clear update flag after processing (need mutable access)
        const_cast<FloatingIsland&>(island).needsPhysicsUpdate = false;
    }
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
            registerChunkWithRenderer(chunk, island, originChunk);
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
        
        // Register chunk with renderer
        if (g_instancedQuadRenderer) {
            glm::mat4 chunkTransform = island->getChunkTransform(chunkCoord);
            g_instancedQuadRenderer->registerChunk(chunk, chunkTransform);
        }
        
        // Queue mesh generation for entire chunk
        if (g_greedyMeshQueue)
        {
            g_greedyMeshQueue->queueChunkMesh(chunk);
        }
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
