// GameServer.h - Headless game server for MMORPG
// This class runs the authoritative game simulation without graphics
#pragma once

#include "GameState.h"
#include "../Time/TimeManager.h"
#include "../Network/NetworkManager.h"  // Re-enabled with ENet integration
#include "../Network/NetworkMessages.h"  // For WorldStateMessage
#include <memory>
#include <atomic>
#include <thread>

/**
 * GameServer runs the authoritative game simulation in a headless environment.
 * It manages the game world state and can handle multiple clients (future).
 * 
 * Key features:
 * - Headless operation (no graphics/window dependencies)
 * - Fixed timestep simulation for deterministic behavior
 * - Thread-safe design for network integration
 * - Separation of simulation from presentation
 */
class GameServer {
public:
    GameServer();
    ~GameServer();
    
    // ================================
    // SERVER LIFECYCLE
    // ================================
    
    /**
     * Initialize the game server
     * @param targetTickRate - Server simulation frequency (default: 60 Hz)
     * @param enableNetworking - Whether to start network server (default: false)
     * @param networkPort - Port for network server (default: 7777)
     */
    bool initialize(float targetTickRate = 60.0f, bool enableNetworking = false, uint16_t networkPort = 7777);
    
    /**
     * Start the server simulation loop
     * This will run in the current thread until stop() is called
     */
    void run();
    
    /**
     * Start the server simulation in a background thread
     */
    void runAsync();
    
    /**
     * Stop the server simulation
     */
    void stop();
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    // ================================
    // SERVER STATE ACCESS
    // ================================
    
    /**
     * Get read-only access to game state (thread-safe)
     */
    const GameState* getGameState() const { return m_gameState.get(); }
    
    /**
     * Get mutable access to game state (for integrated mode)
     */
    GameState* getGameState() { return m_gameState.get(); }
    
    /**
     * Check if server is currently running
     */
    bool isRunning() const { return m_running.load(); }
    
    /**
     * Get server statistics
     */
    float getCurrentTickRate() const { return m_currentTickRate; }
    uint64_t getTotalTicks() const { return m_totalTicks; }
    
    // ================================
    // GAME COMMANDS (Thread-safe)
    // ================================
    
    /**
     * Queue a voxel change command
     * These will be processed on the next server tick
     */
    void queueVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType);
    
    /**
     * Queue a player movement command
     */
    void queuePlayerMovement(const Vec3& movement);
    
private:
    // ================================
    // NETWORKING HELPERS
    // ================================
    
    /**
     * Send world state to a newly connected client
     */
    void sendWorldStateToClient(ENetPeer* peer);
    
    /**
     * Handle voxel change requests from clients
     */
    void handleVoxelChangeRequest(ENetPeer* peer, const VoxelChangeRequest& request);
    
    /**
     * Handle piloting input from clients (server-authoritative)
     */
    void handlePilotingInput(ENetPeer* peer, const PilotingInputMessage& input);
    
    /**
     * Broadcast island state updates to all connected clients
     */
    void broadcastIslandStates();
    
    // Core systems
    std::unique_ptr<GameState> m_gameState;
    std::unique_ptr<TimeManager> m_timeManager;
    std::unique_ptr<NetworkManager> m_networkManager;  // Re-enabled with ENet integration
    
    // Server-side physics (separate from client)
    PhysicsSystem m_serverPhysics;
    
    // Networking
    bool m_networkingEnabled = false;
    
    // Threading
    std::atomic<bool> m_running{false};
    std::unique_ptr<std::thread> m_serverThread;
    
    // Simulation timing
    float m_targetTickRate = 60.0f;
    float m_fixedDeltaTime = 1.0f / 60.0f;
    float m_currentTickRate = 0.0f;
    uint64_t m_totalTicks = 0;
    
    // Command queues (for thread-safe input)
    struct VoxelChangeCommand {
        uint32_t islandID;
        Vec3 localPos;
        uint8_t voxelType;
    };
    
    struct PlayerMovementCommand {
        Vec3 movement;
    };
    
    std::vector<VoxelChangeCommand> m_pendingVoxelChanges;
    std::vector<PlayerMovementCommand> m_pendingPlayerMovements;
    
    // ================================
    // INTERNAL METHODS
    // ================================
    
    /**
     * Main server simulation loop
     */
    void serverLoop();
    
    /**
     * Process one server tick
     */
    void processTick(float deltaTime);
    
    /**
     * Process queued commands from clients
     */
    void processQueuedCommands();
    
    /**
     * Calculate and update tick rate statistics
     */
    void updateTickRateStats(float actualDeltaTime);
};
