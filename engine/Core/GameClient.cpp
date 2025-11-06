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

#include "GameState.h"
#include "../Profiling/Profiler.h"
#include "../World/BlockType.h"

#include "../Network/NetworkManager.h"
#include "../Network/NetworkMessages.h"
#include "../Rendering/BlockHighlightRenderer.h"
#include "../Rendering/GBuffer.h"
#include "../Rendering/DeferredLightingPass.h"
#include "../UI/HUD.h"
#include "../UI/PeriodicTableUI.h"  // NEW: Periodic table UI for hotbar binding
#include "../Core/Window.h"
#include "../Rendering/InstancedQuadRenderer.h"
#include "../Rendering/ModelInstanceRenderer.h"
#include "../Rendering/TextureManager.h"
#include "../Rendering/CascadedShadowMap.h"
#include "../Physics/PhysicsSystem.h"  // For ground detection
#include "../World/AsyncMeshGenerator.h"  // Async mesh generation
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "../Time/TimeEffects.h"
#include "../Time/DayNightController.h"
#include "../World/VoxelChunk.h"  // For accessing voxel data

// External systems
extern TimeEffects* g_timeEffects;
extern ShadowMap g_shadowMap;

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

    // Initialize async mesh generator for non-blocking chunk loading
    if (!g_asyncMeshGenerator)
    {
        g_asyncMeshGenerator = new AsyncMeshGenerator();
    }

    m_initialized = true;
    return true;
}

bool GameClient::connectToGameState(GameState* gameState)
{
    if (!gameState)
    {
        std::cerr << "Cannot connect to null game state!" << std::endl;
        return false;
    }

    m_gameState = gameState;
    m_isRemoteClient = false;  // Local connection

    // **NEW: Connect physics system to island system for collision detection**
    if (gameState && gameState->getIslandSystem())
    {
        m_clientPhysics.setIslandSystem(gameState->getIslandSystem());
        // Mark chunks as client-side (need GPU upload)
        gameState->getIslandSystem()->setIsClient(true);
    }

    // Use calculated spawn position from world generation
    Vec3 playerSpawnPos = gameState->getPlayerSpawnPosition();
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
        PROFILE_SCOPE("NetworkManager::update");
        m_networkManager->update();
    }

    // Process completed async mesh generations (fast - just swaps data)
    if (g_asyncMeshGenerator)
    {
        PROFILE_SCOPE("processCompletedMeshes");
        g_asyncMeshGenerator->processCompletedMeshes();
    }

    // Update client-side physics for smooth island movement
    if (m_gameState)
    {
        PROFILE_SCOPE("updateIslandPhysics");
        auto* islandSystem = m_gameState->getIslandSystem();
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
        PROFILE_SCOPE("DayNightController::update");
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

    // Update profiler (will auto-report every second)
    g_profiler.updateAndReport();

    return true;
}

void GameClient::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    // Shutdown async mesh generator
    if (g_asyncMeshGenerator)
    {
        delete g_asyncMeshGenerator;
        g_asyncMeshGenerator = nullptr;
    }

    // Disconnect from game state
    m_gameState = nullptr;

    // Cleanup renderers (unique_ptr handles deletion automatically)
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
    if (m_gameState)
    {
        // Tell PlayerController if UI is blocking input
        bool uiBlocking = (m_periodicTableUI && m_periodicTableUI->isOpen());
        m_playerController.setUIBlocking(uiBlocking);
        
        // Process mouse input
        m_playerController.processMouse(m_window->getHandle());
        
        // Update player controller (physics and camera)
        m_playerController.update(m_window->getHandle(), deltaTime, m_gameState->getIslandSystem(), &m_clientPhysics);
        
        // Send movement to server if remote client
        if (m_isRemoteClient && m_networkManager)
        {
            Vec3 pos = m_playerController.getPosition();
            Vec3 vel = m_playerController.getVelocity();
            m_networkManager->sendPlayerMovement(pos, vel, deltaTime);
        }
    }

    // Process block interaction
    if (m_gameState)
    {
        processBlockInteraction(deltaTime);
    }
}

void GameClient::render()
{
    PROFILE_SCOPE("GameClient::render");
    
    // Clear depth buffer only (gradient sky will be rendered by deferred lighting shader)
    glClear(GL_DEPTH_BUFFER_BIT);

    // Set up 3D projection
    {
        PROFILE_SCOPE("Setup 3D projection");
        
        float aspect = (float) m_windowWidth / (float) m_windowHeight;
        float fov = 45.0f;
        
        // Update frustum culling
        m_frustumCuller.updateFromCamera(m_playerController.getCamera(), aspect, fov);
    }

    // Render world (only if we have local game state)
    if (m_gameState)
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
    }
    
    // Initialize instanced quad renderer (99.4% memory reduction vs per-vertex approach)
    g_instancedQuadRenderer = std::make_unique<InstancedQuadRenderer>();
    if (!g_instancedQuadRenderer->initialize())
    {
        std::cerr << "❌ Failed to initialize InstancedQuadRenderer!" << std::endl;
        g_instancedQuadRenderer.reset();
        return false;
    }
    std::cout << "✅ InstancedQuadRenderer initialized - shared unit quad ready!" << std::endl;

    // Initialize shadow map system (must happen before renderers that use it)
    if (!g_shadowMap.initialize(16384, 2))
    {
        std::cerr << "❌ Failed to initialize shadow map system!" << std::endl;
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
    if (!m_gameState)
    {
        return;
    }

    // Update raycast timer for performance
    m_inputState.raycastTimer += deltaTime;
    if (m_inputState.raycastTimer > 0.05f)
    {  // 20 FPS raycasting for more responsive block selection
        m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
            m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_gameState->getIslandSystem());
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
            // OPTIMISTIC UPDATE: Apply change immediately on client
            m_gameState->setVoxel(m_inputState.cachedTargetBlock.islandID,
                                  m_inputState.cachedTargetBlock.localBlockPos, 0);
            
            // Send network request for server validation
            if (m_networkManager && m_networkManager->getClient() &&
                m_networkManager->getClient()->isConnected())
            {
                m_networkManager->getClient()->sendVoxelChangeRequest(
                    m_inputState.cachedTargetBlock.islandID,
                    m_inputState.cachedTargetBlock.localBlockPos, 0);
            }

            // Server will confirm or revert via VoxelChangeUpdate

            // Clear the cached target block immediately to remove the yellow outline
            m_inputState.cachedTargetBlock = RayHit();

            // **FIXED**: Force immediate raycast to update block selection after breaking
            m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
                m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_gameState->getIslandSystem());
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
                m_gameState->getVoxel(m_inputState.cachedTargetBlock.islandID, placePos);

            if (existingVoxel == 0)
            {
                // Use locked recipe block
                uint8_t blockToPlace = m_lockedRecipe->blockID;
                
                // OPTIMISTIC UPDATE: Apply change immediately on client
                m_gameState->setVoxel(m_inputState.cachedTargetBlock.islandID, placePos, blockToPlace);
                
                // Send network request for server validation
                if (m_networkManager && m_networkManager->getClient() &&
                    m_networkManager->getClient()->isConnected())
                {
                    m_networkManager->getClient()->sendVoxelChangeRequest(
                        m_inputState.cachedTargetBlock.islandID, placePos, blockToPlace);
                }

                // Server will confirm or revert via VoxelChangeUpdate
                
                // Keep recipe locked for continuous placement
                std::cout << "Block placed (" << m_lockedRecipe->name << " still locked)" << std::endl;

                // Clear the cached target block to refresh the selection
                m_inputState.cachedTargetBlock = RayHit();

                // **FIXED**: Force immediate raycast to update block selection after placing
                m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
                    m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_gameState->getIslandSystem());
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
    
    if (!m_gameState)
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

    // Shadow depth pass (throttled - only update every Nth frame for performance)
    m_frameCounter++;
    if (m_frameCounter % m_shadowUpdateInterval == 0)
    {
        renderShadowPass();
    }

    // === DEFERRED RENDERING PIPELINE ===
    
    // 1. G-Buffer Pass: Render scene geometry to G-buffer (albedo, normal, position, metadata)
    {
        PROFILE_SCOPE("GBuffer_Pass");
        
        g_gBuffer.bindForGeometryPass();
        
        glm::mat4 viewProjection = projectionMatrix * viewMatrix;
        
        // Render voxel quads to G-buffer
        if (g_instancedQuadRenderer)
        {
            g_instancedQuadRenderer->renderToGBuffer(viewProjection, viewMatrix);
        }
        
        // Render GLB models to G-buffer
        if (g_modelRenderer)
        {
            g_modelRenderer->renderToGBuffer(viewMatrix, projectionMatrix);
        }
        
        g_gBuffer.unbind();
    }
    
    // 2. Lighting Pass: Read G-buffer, apply shadows + lighting, output final color to screen
    {
        PROFILE_SCOPE("Deferred_Lighting_Pass");
        
        // Get sun direction for lighting
        Vec3 sunDir = m_dayNightController ? m_dayNightController->getSunDirection() : Vec3(-0.3f, -1.0f, -0.2f).normalized();
        glm::vec3 sunDirGLM(sunDir.x, sunDir.y, sunDir.z);
        
        // Ambient strength from day/night cycle
        float ambientStrength = m_dayNightController ? m_dayNightController->getAmbientIntensity() : 0.3f;
        
        // Update cascade data in deferred lighting pass
        for (int i = 0; i < g_shadowMap.getNumCascades(); ++i)
        {
            const CascadeData& cascade = g_shadowMap.getCascade(i);
            g_deferredLighting.setCascadeData(i, cascade.viewProj, cascade.splitDistance);
        }
        
        // Render full-screen quad with deferred lighting
        g_deferredLighting.render(sunDirGLM, ambientStrength);
    }
    
    // 3. Forward Pass: Render transparent/special objects (block highlight, UI)
    {
        PROFILE_SCOPE("Forward_Pass");
        
        // Render block highlight (yellow wireframe cube on selected block)
        if (m_blockHighlighter && m_inputState.cachedTargetBlock.hit)
        {
            PROFILE_SCOPE("renderBlockHighlight");
            
            auto& islands = m_gameState->getIslandSystem()->getIslands();
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

void GameClient::renderShadowPass()
{
    PROFILE_SCOPE("GameClient::renderShadowPass");
    
    // Use dynamic sun direction from DayNightController for shadows
    Vec3 sunDir = m_dayNightController ? m_dayNightController->getSunDirection() : Vec3(-0.3f, -1.0f, -0.2f).normalized();
    glm::vec3 camPos(m_playerController.getCamera().position.x, m_playerController.getCamera().position.y, m_playerController.getCamera().position.z);
    glm::vec3 lightDir(sunDir.x, sunDir.y, sunDir.z);
    
    int numCascades = g_shadowMap.getNumCascades();
    
    // Cascade configuration for proper coverage with 32-block overlap
    // Cascade 0 (near): covers 0-128 blocks from camera, 256x256 units ortho (high detail)
    // Cascade 1 (far):  covers 0-1000 blocks from camera (render distance), 2048x2048 units ortho
    // 28-block overlap zone from 100-128 blocks for smooth transitions
    const float cascade0Split = 128.0f;   // Near cascade max distance
    const float cascade1Split = 1000.0f;  // Far cascade = camera far plane (render distance)
    
    const float nearOrthoSize = 128.0f;   // Near: 256x256 units coverage
    const float farOrthoSize = 1024.0f;   // Far: 2048x2048 units coverage
    
    // Render each cascade
    for (int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx)
    {
        // Select cascade parameters based on index
        float splitDistance = (cascadeIdx == 0) ? cascade0Split : cascade1Split;
        float orthoSize = (cascadeIdx == 0) ? nearOrthoSize : farOrthoSize;
        float nearPlane = 1.0f;
        float farPlane = splitDistance + 50.0f;  // Extra depth for light frustum
        
        // Build light view matrix centered on camera
        glm::vec3 lightTarget = camPos;
        glm::vec3 lightPos = camPos - lightDir * (farPlane * 0.5f);
        glm::mat4 lightView = glm::lookAt(lightPos, lightTarget, glm::vec3(0,1,0));
        
        // Build light projection with texel snapping for stability
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);
        
        // Snap to texel grid to prevent shadow shimmering
        glm::vec4 centerLS = lightView * glm::vec4(lightTarget, 1.0f);
        int smWidth = g_shadowMap.getSize();
        float texelSize = (2.0f * orthoSize) / float(smWidth);
        glm::vec2 snapped = glm::floor(glm::vec2(centerLS.x, centerLS.y) / texelSize) * texelSize;
        glm::vec2 delta = snapped - glm::vec2(centerLS.x, centerLS.y);
        glm::mat4 snapMat = glm::translate(glm::mat4(1.0f), glm::vec3(-delta.x, -delta.y, 0.0f));
        glm::mat4 lightVP = lightProj * snapMat * lightView;
        
        // Store cascade data for shader (splitDistance is the MAX view-space depth for this cascade)
        CascadeData cascadeData;
        cascadeData.viewProj = lightVP;
        cascadeData.splitDistance = splitDistance;  // Shader compares fragment depth against this
        cascadeData.orthoSize = orthoSize;
        g_shadowMap.setCascadeData(cascadeIdx, cascadeData);
        
        // Render shadow depth pass for this cascade
        if (m_windowWidth > 0 && m_windowHeight > 0)
        {
            // Begin shadow map rendering (clears depth buffer, sets up FBO)
            g_shadowMap.begin(cascadeIdx);
            
            // Render voxels into shadow map (they cast shadows!)
            if (g_instancedQuadRenderer)
            {
                g_instancedQuadRenderer->beginDepthPass(lightVP, cascadeIdx);
                g_instancedQuadRenderer->renderDepth();
                g_instancedQuadRenderer->endDepthPass(m_windowWidth, m_windowHeight);
            }
            
            // Render GLB models into shadow map
            if (g_modelRenderer) {
                g_modelRenderer->beginDepthPass(lightVP, cascadeIdx);
                g_modelRenderer->renderDepth();
                g_modelRenderer->endDepthPass(m_windowWidth, m_windowHeight);
            }
            
            // End shadow map rendering (restores viewport/framebuffer)
            g_shadowMap.end(m_windowWidth, m_windowHeight);
        }
    }
    
    // Restore culling for forward rendering pass (shadow pass disabled it)
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    // Set lighting data for forward pass (use first cascade for now - will update shader to pick cascade)
    glm::vec3 lightDirVec(lightDir.x, lightDir.y, lightDir.z);
    if (g_modelRenderer)
    {
        g_modelRenderer->setLightingData(g_shadowMap.getCascade(0).viewProj, lightDirVec);
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
        if (m_inputState.cachedTargetBlock.hit && m_gameState)
        {
            uint8_t blockID = m_gameState->getVoxel(
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
}

void GameClient::handleWorldStateReceived(const WorldStateMessage& worldState)
{
    // Create a new GameState for the client based on server data
    m_gameState = new GameState();

    if (!m_gameState->initialize(false))
    {  // Don't create default world, we'll use server data
        std::cerr << "Failed to initialize client game state!" << std::endl;
        delete m_gameState;
        m_gameState = nullptr;
        return;
    }

    // Connect physics system to CLIENT's island system for collision detection
    if (m_gameState && m_gameState->getIslandSystem())
    {
        m_clientPhysics.setIslandSystem(m_gameState->getIslandSystem());
        // Mark chunks as client-side (need GPU upload)
        m_gameState->getIslandSystem()->setIsClient(true);
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
    if (!m_gameState || !g_instancedQuadRenderer)
        return;
    
    auto* islandSystem = m_gameState->getIslandSystem();
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
        
        // Calculate island transform once (includes rotation + translation)
        glm::mat4 islandTransform = island.getTransformMatrix();
        
        // Update transforms for all chunks in this island
        for (auto& [chunkCoord, chunk] : island.chunks)
        {
            if (!chunk) continue;
            
            // Use helper to compute chunk transform
            glm::mat4 chunkTransform = island.getChunkTransform(chunkCoord);
            
            // === UPDATE INSTANCED RENDERER (voxel chunks) ===
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
    if (!m_gameState)
    {
        std::cerr << "Cannot handle island data: No game state initialized" << std::endl;
        return;
    }

    auto* islandSystem = m_gameState->getIslandSystem();
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
            chunk->generateMesh();  // Already builds collision mesh internally
            
            // Register chunk with renderer
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
    if (!m_gameState)
    {
        std::cerr << "Cannot handle chunk data: No game state initialized" << std::endl;
        return;
    }

    auto* islandSystem = m_gameState->getIslandSystem();
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
        
        // IMMEDIATELY register chunk with renderer (callback must be set before any modifications)
        registerChunkWithRenderer(chunk, island, chunkCoord);
        
        // Queue async mesh generation (will update the already-registered chunk)
        if (g_asyncMeshGenerator)
        {
            g_asyncMeshGenerator->queueChunkMeshGeneration(chunk, nullptr);
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
    if (!m_gameState)
    {
        std::cerr << "Cannot apply voxel change: no game state!" << std::endl;
        return;
    }

    // Apply the authoritative voxel change from server
    // This uses incremental quad updates (addBlockQuads/removeBlockQuads) automatically
    m_gameState->setVoxel(update.islandID, update.localPos, update.voxelType);

    // Incremental updates already handled by setVoxel() - no need to queue mesh generation!
    // The quad modifications are immediate and GPU upload happens on next render frame.

    // **FIXED**: Always force immediate raycast update when server sends voxel changes
    // This ensures block selection is immediately accurate after server updates
    m_inputState.cachedTargetBlock = VoxelRaycaster::raycast(
        m_playerController.getCamera().position, m_playerController.getCamera().front, 50.0f, m_gameState->getIslandSystem());
    m_inputState.raycastTimer = 0.0f;
}

void GameClient::handleEntityStateUpdate(const EntityStateUpdate& update)
{
    if (!m_gameState)
    {
        return;
    }

    // Handle different entity types
    switch (update.entityType)
    {
        case 1:
        {  // Island
            auto* islandSystem = m_gameState->getIslandSystem();
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
                }
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
