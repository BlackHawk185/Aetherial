// GameClient.h - Client-side rendering and input handling
// This class manages the presentation layer and user interaction
#pragma once

#include "../Math/Vec3.h"
#include "../Input/PlayerController.h"
#include "../World/VoxelRaycaster.h"
#include "../World/ElementRecipes.h"  // NEW: Element-based crafting system
#include "../Network/NetworkManager.h"  // Re-enabled with ENet integration working
#include "../Time/DayNightController.h"  // NEW: Simplified day/night cycle
#include <memory>
#include <string>
#include <array>
#include <unordered_map>

// Forward declarations
class ClientWorld;
class BlockHighlightRenderer;
class HUD;
class PeriodicTableUI;
class VoxelChunk;
struct GLFWwindow;
struct VoxelChangeUpdate;
struct WorldStateMessage;

namespace Engine { namespace Core { class Window; } }
namespace Engine { namespace Rendering { class VoxelRenderer; } }

/**
 * GameClient handles the presentation layer of the game.
 * It manages rendering, input, and UI, but does not own the game state.
 * 
 * The client can either:
 * 1. Connect to a local GameServer (integrated mode)
 * 2. Connect to a remote server (client-only mode)
 * 3. Work with a shared ClientWorld directly (current transition mode)
 */

/**
 * GameClient handles the presentation layer of the game.
 * It manages rendering, input, and UI, but does not own the game state.
 * 
 * The client can either:
 * 1. Connect to a local GameServer (integrated mode)
 * 2. Connect to a remote server (client-only mode)
 * 3. Work with a shared ClientWorld directly (current transition mode)
 */
class GameClient {
public:
    GameClient();
    ~GameClient();
    
    // ================================
    // CLIENT LIFECYCLE
    // ================================
    
    /**
     * Initialize the client (creates window, graphics context, etc.)
     */
    bool initialize(bool enableDebug = false);
    
    /**
     * Connect to a client world (local or remote)
     * @param clientWorld - The client world to render (can be local)
     */
    bool connectToClientWorld(ClientWorld* clientWorld);
    
    /**
     * Connect to a remote server
     * @param serverAddress - The server address to connect to
     * @param serverPort - The server port to connect to
     */
    bool connectToRemoteServer(const std::string& serverAddress, uint16_t serverPort);
    
    /**
     * Main client loop - handles input, rendering, and presentation
     * Returns false when the client should exit
     */
    bool update(float deltaTime);
    
    /**
     * Shutdown the client
     */
    void shutdown();
    
    // ================================
    // INPUT HANDLING
    // ================================
    
    /**
     * Process input and generate commands
     * These commands will be sent to the game state/server
     */
    void processInput(float deltaTime);
    
    /**
     * Check if the client window should close
     */
    bool shouldClose() const;
    
    // ================================
    // RENDERING
    // ================================
    
    /**
     * Render the current game state
     */
    void render();
    
    /**
     * Get the current camera for external use
     */
    Camera& getCamera() { return m_playerController.getCamera(); }
    const Camera& getCamera() const { return m_playerController.getCamera(); }
    
    /**
     * Get the player controller for external access
     */
    PlayerController& getPlayerController() { return m_playerController; }
    const PlayerController& getPlayerController() const { return m_playerController; }
    
private:
    // Graphics window/context
    std::unique_ptr<Engine::Core::Window> m_window;
    int m_windowWidth = 1280;
    int m_windowHeight = 720;
    
    // Client world connection
    ClientWorld* m_clientWorld = nullptr;  // Not owned by client
    
    // Client-side physics (separate from server)
    PhysicsSystem m_clientPhysics;
    
    // Networking - Re-enabled with ENet integration
    std::unique_ptr<NetworkManager> m_networkManager;
    bool m_isRemoteClient = false;
    
    // Player control system (unified input, physics, and camera)
    PlayerController m_playerController;
    std::unique_ptr<BlockHighlightRenderer> m_blockHighlighter;
    std::unique_ptr<HUD> m_hud;
    std::unique_ptr<PeriodicTableUI> m_periodicTableUI;
    
    // NEW: Day/night cycle (atmospheric colors rendered in deferred shader)
    std::unique_ptr<DayNightController> m_dayNightController;
    
    // FPS tracking
    float m_lastFrameDeltaTime = 0.016f; // Start at ~60 FPS
    
    // Shadow update throttling - render shadows every Nth frame
    uint32_t m_frameCounter = 0;
    uint32_t m_shadowUpdateInterval = 1;  // Update shadows every frame (was 2 - caused grain)
    
    // Input state
    struct InputState {
        bool leftMousePressed = false;
        bool rightMousePressed = false;
        float raycastTimer = 0.0f;
        RayHit cachedTargetBlock;  // Cache raycast results for performance
    } m_inputState;
    
    // Client-side prediction tracking
    struct PendingVoxelChange {
        uint32_t islandID;
        Vec3 localPos;
        uint8_t predictedType;
        uint8_t previousType;  // For rollback if server rejects
    };
    std::unordered_map<uint32_t, PendingVoxelChange> m_pendingVoxelChanges;  // sequenceNumber -> change
    
    // NEW: Element-based crafting system
    ElementQueue m_elementQueue;
    const BlockRecipe* m_lockedRecipe = nullptr;  // Recipe locked in for placement
    std::array<Element, 9> m_hotbarElements;  // Customizable hotbar (keys 1-9)
    
    // Client state
    bool m_initialized = false;
    bool m_debugMode = false;
    
    // ================================
    // INTERNAL METHODS
    // ================================
    
    /**
     * Initialize GLFW and create window
     */
    bool initializeWindow();
    
    /**
     * Initialize graphics systems (ImGui, etc.)
     */
    bool initializeGraphics();
    
    /**
     * Process keyboard and mouse input
     */
    void processKeyboard(float deltaTime);
    void processBlockInteraction(float deltaTime);
    
    /**
     * Render the 3D world
     */
    void renderWorld();
    
    /**
     * Render shadow depth pass for cascaded shadow mapping
     */
    void renderLightDepthPass(const std::vector<VoxelChunk*>& visibleChunks);
    
    /**
     * Handle received world state from server
     */
    void handleWorldStateReceived(const WorldStateMessage& worldState);
    
    /**
     * Handle received compressed island data from server
     */
    void handleCompressedIslandReceived(uint32_t islandID, const Vec3& position, const uint8_t* voxelData, uint32_t dataSize);
    
    /**
     * Handle received compressed chunk data from server
     */
    void handleCompressedChunkReceived(uint32_t islandID, const Vec3& chunkCoord, const Vec3& islandPosition, const uint8_t* voxelData, uint32_t dataSize);
    
    /**
     * Handle received voxel change updates from server
     */
    void handleVoxelChangeReceived(const VoxelChangeUpdate& update);
    
    /**
     * Handle received entity state updates from server
     */
    void handleEntityStateUpdate(const EntityStateUpdate& update);
    
    /**
     * Handle fluid particle spawn from server
     */
    void handleFluidParticleSpawn(const FluidParticleSpawnMessage& msg);
    
    /**
     * Handle fluid particle despawn from server
     */
    void handleFluidParticleDespawn(const FluidParticleDespawnMessage& msg);
    
    /**
     * Helper: Register a chunk with the renderer and set up callbacks
     */
    void registerChunkWithRenderer(VoxelChunk* chunk, FloatingIsland* island, const Vec3& chunkCoord);
    
    /**
     * Sync island transforms to chunk renderers (event-driven, only updates moved islands)
     */
    void syncPhysicsToChunks();
    
    /**
     * CRITICAL: Centralized spawn function - ONLY place where player position should be set
     * This ensures m_camera.position and m_physicsPosition stay in sync
     */
    void spawnPlayerAt(const Vec3& worldPosition);
    
    /**
     * Render waiting screen for remote clients
     */
    void renderWaitingScreen();
    
    /**
     * Render UI and debug info
     */
    void renderUI();
    
    /**
     * Handle window resize
     */
    void onWindowResize(int width, int height);

    // Lighting neighborhood tracking
    Vec3 m_lastChunkCoord{999999,999999,999999};

}; 
