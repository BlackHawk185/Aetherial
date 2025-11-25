// GameServer.cpp - Headless game server implementation
#include "GameServer.h"

#include "pch.h"
#include "../Profiling/Profiler.h"

#include "../Network/NetworkMessages.h"
#include "../World/VoxelChunk.h"  // For accessing voxel data
#include "../World/ConnectivityAnalyzer.h"  // For island splitting
#include "../ECS/ECS.h"  // For fluid particle ECS access
#include "../World/FluidComponents.h"  // For FluidParticleComponent

extern ECSWorld g_ecs;  // Global ECS instance

GameServer::GameServer()
{
    // Constructor
    m_networkManager = std::make_unique<NetworkManager>();  // Re-enabled with ENet
}

GameServer::~GameServer()
{
    shutdown();
}

bool GameServer::initialize(float targetTickRate, bool enableNetworking, uint16_t networkPort)
{
    // Removed verbose debug output

    m_targetTickRate = targetTickRate;
    m_fixedDeltaTime = 1.0f / targetTickRate;
    m_networkingEnabled = enableNetworking;

    // Initialize time manager
    m_timeManager = std::make_unique<TimeManager>();

    // Initialize server world
    m_serverWorld = std::make_unique<ServerWorld>();
    if (!m_serverWorld->initialize())
    {  // Create default world
        std::cerr << "Failed to initialize server world!" << std::endl;
        return false;
    }
    
    // Initialize player position for integrated mode
    m_lastKnownPlayerPosition = m_serverWorld->getPlayerSpawnPosition();
    m_hasPlayerPosition = true;

    // Log island generation mode once (noise is now default)
    // Connect physics system to island system for server-side collision detection
    m_serverPhysics.setIslandSystem(m_serverWorld->getIslandSystem());
    
    // Connect to the already-initialized fluid system
    m_fluidSystem = &g_fluidSystem;
    
    // Setup fluid system callbacks for network broadcasting
    setupFluidSystemCallbacks();

    // Initialize networking if requested
    if (m_networkingEnabled)
    {
        if (!m_networkManager->initializeNetworking())
        {
            std::cerr << "Failed to initialize networking!" << std::endl;
            return false;
        }

        if (!m_networkManager->startHosting(networkPort))
        {
            std::cerr << "Failed to start network server on port " << networkPort << std::endl;
            return false;
        }

        // Set up callback to send world state to new clients
        if (auto server = m_networkManager->getServer())
        {
            server->onClientConnected = [this](ENetPeer* peer)
            {
                // Removed verbose debug output
                sendWorldStateToClient(peer);
            };

            server->onVoxelChangeRequest = [this](ENetPeer* peer, const VoxelChangeRequest& request)
            { this->handleVoxelChangeRequest(peer, request); };

            server->onPilotingInput = [this](ENetPeer* peer, const PilotingInputMessage& input)
            { this->handlePilotingInput(peer, input); };
            
            // Track player position for island activation
            server->onPlayerMovementRequest = [this](ENetPeer* /* peer */, const PlayerMovementRequest& request)
            {
                m_lastKnownPlayerPosition = request.intendedPosition;
                m_hasPlayerPosition = true;
            };
        }

        // Removed verbose debug output
    }

    // Removed verbose debug output

    return true;
}

void GameServer::run()
{
    if (m_running.load())
    {
        std::cerr << "Server is already running!" << std::endl;
        return;
    }

    // Removed verbose debug output
    m_running.store(true);

    serverLoop();
}

void GameServer::runAsync()
{
    if (m_running.load())
    {
        std::cerr << "Server is already running!" << std::endl;
        return;
    }

    // Removed verbose debug output
    m_running.store(true);

    m_serverThread = std::make_unique<std::thread>(&GameServer::serverLoop, this);
}

void GameServer::stop()
{
    if (!m_running.load())
    {
        return;
    }

    std::cout << "⏹️  Stopping GameServer..." << std::endl;
    m_running.store(false);

    // Wait for server thread to finish
    if (m_serverThread && m_serverThread->joinable())
    {
        m_serverThread->join();
        m_serverThread.reset();
    }
}

void GameServer::shutdown()
{
    stop();

    // Clear command queues
    m_pendingVoxelChanges.clear();
    m_pendingPlayerMovements.clear();

    // Shutdown systems
    if (m_serverWorld)
    {
        m_serverWorld->shutdown();
        m_serverWorld.reset();
    }

    m_timeManager.reset();
}

void GameServer::queueVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType)
{
    // TODO: Add proper thread-safe queue implementation
    // For now, just apply directly (not thread-safe!)
    VoxelChangeCommand cmd;
    cmd.islandID = islandID;
    cmd.localPos = localPos;
    cmd.voxelType = voxelType;
    m_pendingVoxelChanges.push_back(cmd);
}

void GameServer::queuePlayerMovement(const Vec3& movement)
{
    // TODO: Add proper thread-safe queue implementation
    PlayerMovementCommand cmd;
    cmd.movement = movement;
    m_pendingPlayerMovements.push_back(cmd);
}

void GameServer::serverLoop()
{
    PROFILE_SCOPE("GameServer::serverLoop");
    
    auto lastTime = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;

    // Removed verbose debug output

    while (m_running.load())
    {
        PROFILE_SCOPE("Server main loop iteration");
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        // Clamp delta time to prevent spiral of death
        if (deltaTime > 0.25f)
        {
            deltaTime = 0.25f;
        }

        accumulator += deltaTime;

        // Fixed timestep simulation
        while (accumulator >= m_fixedDeltaTime)
        {
            PROFILE_SCOPE("Fixed timestep tick");
            processTick(m_fixedDeltaTime);
            accumulator -= m_fixedDeltaTime;
            m_totalTicks++;
        }

        // Update tick rate statistics
        updateTickRateStats(deltaTime);

        std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }

    // Removed verbose debug output
}

void GameServer::processTick(float deltaTime)
{
    PROFILE_SCOPE("GameServer::processTick");
    
    // Process queued commands first
    processQueuedCommands();
    
    // Process pending split checks (expensive, runs on game thread not network thread)
    processPendingSplits();

    // Update networking
    if (m_networkingEnabled && m_networkManager)
    {
        m_networkManager->update();
    }

    // Update time manager
    if (m_timeManager)
    {
        m_timeManager->update(deltaTime);
    }

    // Update game simulation
    if (m_serverWorld)
    {
        m_serverWorld->updatePhysics(deltaTime, &m_serverPhysics);
        m_serverWorld->updateSimulation(deltaTime);
        
        // Update fluid system
        if (m_fluidSystem)
        {
            m_fluidSystem->update(deltaTime);
        }
        
        // Check for island activation based on player position
        if (m_hasPlayerPosition)
        {
            m_serverWorld->updateIslandActivation(m_lastKnownPlayerPosition);
        }
    }

    // Broadcast island state updates to clients
    if (m_networkingEnabled && m_networkManager)
    {
        broadcastIslandStates();
    }
}

void GameServer::processQueuedCommands()
{
    PROFILE_SCOPE("GameServer::processQueuedCommands");
    
    // Process voxel changes - copy to avoid iterator invalidation
    std::vector<VoxelChangeCommand> voxelChanges;
    voxelChanges.swap(m_pendingVoxelChanges);  // Fast swap, clears original
    
    for (const auto& cmd : voxelChanges)
    {
        if (m_serverWorld)
        {
            m_serverWorld->setVoxelAuthoritative(cmd.islandID, cmd.localPos, cmd.voxelType);
            
            // Trigger fluid activation if block is being removed near sleeping fluid
            if (cmd.voxelType == 0 && m_fluidSystem) // Block removal
            {
                // Check for fluid activation with moderate disturbance force
                m_fluidSystem->triggerFluidActivation(cmd.islandID, cmd.localPos, 2.0f);
            }
        }
    }

    // Process player movements - copy to avoid iterator invalidation
    std::vector<PlayerMovementCommand> movements;
    movements.swap(m_pendingPlayerMovements);  // Fast swap, clears original
    
    // Movement is now handled by client-side PlayerController
    // Server receives position updates directly from physics
}

void GameServer::updateTickRateStats(float actualDeltaTime)
{
    // Simple moving average for tick rate calculation
    static float tickRateAccumulator = 0.0f;
    static int tickRateSamples = 0;

    if (actualDeltaTime > 0.0f)
    {
        tickRateAccumulator += 1.0f / actualDeltaTime;
        tickRateSamples++;

        // Update every 60 samples (~1 second)
        if (tickRateSamples >= 60)
        {
            m_currentTickRate = tickRateAccumulator / tickRateSamples;
            tickRateAccumulator = 0.0f;
            tickRateSamples = 0;
        }
    }
}

void GameServer::sendWorldStateToClient(ENetPeer* peer)
{
    if (!m_serverWorld || !m_networkManager)
    {
        std::cerr << "Cannot send world state: missing game state or network manager" << std::endl;
        return;
    }

    auto server = m_networkManager->getServer();
    if (!server)
    {
        std::cerr << "No server instance available" << std::endl;
        return;
    }

    // Get island system from game state
    auto* islandSystem = m_serverWorld->getIslandSystem();
    if (!islandSystem)
    {
        std::cerr << "No island system available" << std::endl;
        return;
    }

    // Create world state message from current game state
    WorldStateMessage worldState;
    worldState.numIslands = 3;  // We know we have 3 islands from createDefaultWorld

    // Get actual island positions from the island system
    const std::vector<uint32_t>& islandIDs = m_serverWorld->getIslandIDs();
    for (size_t i = 0; i < 3 && i < islandIDs.size(); i++)
    {
        Vec3 islandCenter = islandSystem->getIslandCenter(islandIDs[i]);
        worldState.islandPositions[i] = islandCenter;
    }

    // Use the calculated spawn position from world generation
    worldState.playerSpawnPosition = m_serverWorld->getPlayerSpawnPosition();

    // Send basic world state first
    server->sendWorldStateToClient(peer, worldState);

    // Now send compressed voxel data for ALL islands
    std::cout << "[SERVER] Sending " << islandIDs.size() << " islands to client..." << std::endl;
    for (size_t i = 0; i < islandIDs.size(); i++)
    {
        const FloatingIsland* island = islandSystem->getIsland(islandIDs[i]);
        if (island)
        {
            std::cout << "[SERVER] Sending island " << (i+1) << "/" << islandIDs.size() 
                      << " (ID=" << islandIDs[i] << ", " << island->chunks.size() << " chunks)" << std::endl;
            
            // Send all chunks for this island
            for (const auto& [chunkCoord, chunk] : island->chunks)
            {
                if (chunk)
                {
                    const uint8_t* voxelData = chunk->getRawVoxelData();
                    uint32_t voxelDataSize = chunk->getVoxelDataSize();

                    // Use the new sendCompressedChunkToClient method with chunk coordinates
                    server->sendCompressedChunkToClient(peer, islandIDs[i], chunkCoord, worldState.islandPositions[i], voxelData, voxelDataSize);
                }
            }
        }
    }
}

void GameServer::handleVoxelChangeRequest(ENetPeer* peer, const VoxelChangeRequest& request)
{
    (void)peer; // Peer info not needed for voxel changes currently
    
    std::cout << "[SERVER] Received voxel change request: island=" << request.islandID 
              << " pos=(" << request.localPos.x << "," << request.localPos.y << "," << request.localPos.z 
              << ") type=" << (int)request.voxelType << std::endl;

    if (!m_serverWorld)
    {
        std::cerr << "Cannot handle voxel change: no game state!" << std::endl;
        return;
    }

    auto* islandSystem = m_serverWorld->getIslandSystem();
    if (!islandSystem)
    {
        std::cerr << "Cannot handle voxel change: no island system!" << std::endl;
        return;
    }

    // Apply the block change immediately for responsiveness
    m_serverWorld->setVoxelAuthoritative(request.islandID, request.localPos, request.voxelType);

    // Broadcast the change to all connected clients (including the sender for confirmation)
    if (auto server = m_networkManager->getServer())
    {
        server->broadcastVoxelChange(request.islandID, request.localPos, request.voxelType, 0);
    }

    // Queue split check for next tick (don't block network thread!)
    // Only check for block removal, not placement
    bool islandSplittingDisabled = false;
    if (request.voxelType == 0 && !islandSplittingDisabled)
    {
        PendingSplitCheck splitCheck;
        splitCheck.islandID = request.islandID;
        splitCheck.blockPos = request.localPos;
        splitCheck.sequenceNumber = m_totalTicks; // Use tick count as sequence
        m_pendingSplitChecks.push_back(splitCheck);
        std::cout << "[SERVER] Queued split check for island " << request.islandID 
                  << " at (" << request.localPos.x << "," << request.localPos.y << "," << request.localPos.z << ")" << std::endl;
    }
}

void GameServer::processPendingSplits()
{
    if (m_pendingSplitChecks.empty()) return;
    
    auto* islandSystem = m_serverWorld->getIslandSystem();
    if (!islandSystem) return;
    
    // Process all pending split checks (now safe on game thread)
    for (const auto& splitCheck : m_pendingSplitChecks)
    {
        std::cout << "[SERVER] Processing split check for island " << splitCheck.islandID << std::endl;
        
        FloatingIsland* island = islandSystem->getIsland(splitCheck.islandID);
        if (!island)
        {
            std::cerr << "[SERVER] WARNING: Island " << splitCheck.islandID << " no longer exists" << std::endl;
            continue;
        }
        
        // NOTE: Block was already removed, so we need to check neighbors' connectivity
        // Get all solid neighbors around where the block WAS
        std::vector<Vec3> neighbors;
        const Vec3 offsets[6] = {
            Vec3(1,0,0), Vec3(-1,0,0),
            Vec3(0,1,0), Vec3(0,-1,0),
            Vec3(0,0,1), Vec3(0,0,-1)
        };
        
        for (const Vec3& offset : offsets)
        {
            Vec3 neighborPos = splitCheck.blockPos + offset;
            if (ConnectivityAnalyzer::isSolidVoxel(island, neighborPos))
            {
                neighbors.push_back(neighborPos);
            }
        }
        
        if (neighbors.size() < 2)
        {
            continue; // Can't have caused a split with less than 2 neighbors
        }
        
        std::cout << "[SERVER] Block had " << neighbors.size() << " neighbors, checking connectivity..." << std::endl;
        
        try
        {
            Vec3 fragmentAnchor;
            if (ConnectivityAnalyzer::wouldBreakingCauseSplit(island, splitCheck.blockPos, fragmentAnchor))
            {
                std::cout << "🌊 SPLIT DETECTED! Extracting fragment..." << std::endl;
                
                // Extract the fragment to a new island
                std::vector<Vec3> removedVoxels;
                uint32_t newIslandID = ConnectivityAnalyzer::extractFragmentToNewIsland(
                    islandSystem, splitCheck.islandID, fragmentAnchor, &removedVoxels);
                    
                if (newIslandID != 0)
                {
                    std::cout << "✅ Fragment extracted to new island " << newIslandID 
                              << " (" << removedVoxels.size() << " voxels removed from original)" << std::endl;
                    
                    auto server = m_networkManager->getServer();
                    if (server)
                    {
                        // Broadcast all removed voxels from the original island
                        for (const Vec3& removedPos : removedVoxels)
                        {
                            server->broadcastVoxelChange(splitCheck.islandID, removedPos, 0, 0);
                        }
                        
                        // Broadcast new island to all clients
                        const FloatingIsland* newIsland = islandSystem->getIsland(newIslandID);
                        if (newIsland)
                        {
                            std::cout << "📡 Broadcasting new island " << newIslandID 
                                      << " (" << newIsland->chunks.size() << " chunks) to all clients" << std::endl;
                            
                            // Make a copy of connected clients to avoid iterator invalidation
                            auto clients = server->getConnectedClients();
                            
                            // Send all chunks of the new island to all connected clients
                            for (ENetPeer* clientPeer : clients)
                            {
                                // Make a copy of chunk coordinates to avoid iterator invalidation
                                std::vector<Vec3> chunkCoords;
                                chunkCoords.reserve(newIsland->chunks.size());
                                for (const auto& [coord, _] : newIsland->chunks)
                                {
                                    chunkCoords.push_back(coord);
                                }
                                
                                for (const Vec3& chunkCoord : chunkCoords)
                                {
                                    auto it = newIsland->chunks.find(chunkCoord);
                                    if (it != newIsland->chunks.end() && it->second)
                                    {
                                        const uint8_t* voxelData = it->second->getRawVoxelData();
                                        uint32_t voxelDataSize = it->second->getVoxelDataSize();
                                        
                                        server->sendCompressedChunkToClient(
                                            clientPeer, newIslandID, chunkCoord, 
                                            newIsland->physicsCenter, voxelData, voxelDataSize);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                std::cout << "[SERVER] No split detected" << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "❌ Error during split extraction: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "❌ Unknown error during split extraction!" << std::endl;
        }
    }
    
    // Clear processed split checks
    m_pendingSplitChecks.clear();
}

void GameServer::handlePilotingInput(ENetPeer* peer, const PilotingInputMessage& input)
{
    (void)peer; // Peer info not needed currently

    if (!m_serverWorld)
    {
        std::cerr << "Cannot handle piloting input: no game state!" << std::endl;
        return;
    }

    auto* islandSystem = m_serverWorld->getIslandSystem();
    if (!islandSystem)
    {
        std::cerr << "Cannot handle piloting input: no island system!" << std::endl;
        return;
    }

    FloatingIsland* island = islandSystem->getIsland(input.islandID);
    if (!island)
    {
        std::cerr << "Cannot handle piloting input: island " << input.islandID << " not found!" << std::endl;
        return;
    }

    // Apply piloting forces (server-authoritative)
    const float thrustStrength = 5.0f;       // Thrust acceleration
    const float rotationSpeed = 1.0f;        // Rotation speed (radians per second)
    const float deltaTime = 1.0f / 60.0f;    // Assume 60 FPS for now

    // Apply rotation input
    island->angularVelocity.y = input.rotationYaw * rotationSpeed;

    // Apply thrust input
    Vec3 thrustAcceleration(0, input.thrustY * thrustStrength, 0);

    // Apply thrust to island velocity
    island->velocity = island->velocity + thrustAcceleration * deltaTime;

    // Apply damping to prevent runaway velocity
    const float dampingFactor = 0.98f;
    island->velocity.x *= dampingFactor;
    island->velocity.y *= dampingFactor;
    island->velocity.z *= dampingFactor;
    
    island->invalidateTransform();

    // Apply angular damping when no rotation input
    if (input.rotationYaw == 0.0f)
    {
        island->angularVelocity.y *= 0.9f;
    }

    island->needsPhysicsUpdate = true;

    // Server will broadcast updated island state in next broadcastIslandStates() call
}

void GameServer::broadcastIslandStates()
{
    PROFILE_SCOPE("GameServer::broadcastIslandStates");
    
    if (!m_serverWorld || !m_networkManager)
    {
        return;
    }

    auto server = m_networkManager->getServer();
    if (!server)
    {
        return;
    }

    auto* islandSystem = m_serverWorld->getIslandSystem();
    if (!islandSystem)
    {
        return;
    }

    // Broadcast state for all islands at a reasonable frequency (e.g., 10Hz for smooth movement)
    static float lastBroadcastTime = 0.0f;
    float currentTime = m_timeManager ? m_timeManager->getRealTime() : 0.0f;

    if (currentTime - lastBroadcastTime < 0.1f)
    {  // 10Hz update rate
        return;
    }
    lastBroadcastTime = currentTime;

    // Broadcast state for ALL islands (including dynamically created split islands)
    const auto& allIslands = islandSystem->getIslands();
    uint32_t serverTimestamp =
        static_cast<uint32_t>(currentTime * 1000.0f);  // Convert to milliseconds

    for (const auto& [islandID, island] : allIslands)
    {
        // Create EntityStateUpdate for this island
        EntityStateUpdate update;
        update.sequenceNumber = static_cast<uint32_t>(
            m_totalTicks);  // Use tick count as sequence (truncated to 32-bit for network)
        update.entityID = islandID;
        update.entityType = 1;  // 1 = Island (as defined in NetworkMessages.h)
        update.position = island.physicsCenter;
        update.velocity = island.velocity;
        update.acceleration = island.acceleration;
        update.rotation = island.rotation;               // Send rotation state
        update.angularVelocity = island.angularVelocity; // Send angular velocity
        update.serverTimestamp = serverTimestamp;
        update.flags = 0;  // No special flags for islands

        // Broadcast to all connected clients
        server->broadcastEntityState(update);
    }
    
    // TODO: Fluid particle broadcasting disabled - needs ECS refactoring
    /*
    // Broadcast fluid particle states
    auto fluidView = g_ecs.view<FluidParticleComponent, TransformComponent>();
    for (auto entity : fluidView) {
        auto* fluidComp = g_ecs.getComponent<FluidParticleComponent>(entity);
        auto* transform = g_ecs.getComponent<TransformComponent>(entity);
        
        if (!fluidComp || !transform) continue;
        
        EntityStateUpdate update;
        update.sequenceNumber = static_cast<uint32_t>(m_totalTicks);
        update.entityID = static_cast<uint32_t>(entity);
        update.entityType = 3;  // 3 = Fluid Particle
        update.position = transform->position;
        update.velocity = fluidComp->velocity;
        update.acceleration = Vec3(0, 0, 0);  // Fluid uses simple physics
        update.rotation = Vec3(0, 0, 0);
        update.angularVelocity = Vec3(0, 0, 0);
        update.serverTimestamp = serverTimestamp;
        update.flags = 0;
        
        
        server->broadcastEntityState(update);
    }
    */
}

void GameServer::setupFluidSystemCallbacks()
{
    if (!m_fluidSystem)
    {
        return;
    }

    // Setup callback for fluid particle spawn
    m_fluidSystem->setParticleSpawnCallback(
        [this](EntityID entityID, uint32_t islandID, const Vec3& worldPos, 
               const Vec3& velocity, const Vec3& originalVoxelPos)
        {
            broadcastFluidParticleSpawn(entityID, islandID, worldPos, velocity, originalVoxelPos);
        }
    );

    // Setup callback for fluid particle despawn
    m_fluidSystem->setParticleDespawnCallback(
        [this](EntityID entityID, uint32_t islandID, const Vec3& settledVoxelPos, bool shouldCreateVoxel)
        {
            broadcastFluidParticleDespawn(entityID, islandID, settledVoxelPos, shouldCreateVoxel);
        }
    );
    
    // Setup callback for voxel changes from fluid system
    m_fluidSystem->setVoxelChangeCallback(
        [this](uint32_t islandID, const Vec3& position, uint8_t voxelType)
        {
            // Broadcast voxel change to all clients
            if (m_networkingEnabled && m_networkManager)
            {
                if (auto server = m_networkManager->getServer())
                {
                    server->broadcastVoxelChange(islandID, position, voxelType, 0);
                }
            }
        }
    );
}

void GameServer::broadcastFluidParticleSpawn(EntityID entityID, uint32_t islandID, const Vec3& worldPos, 
                                             const Vec3& velocity, const Vec3& originalVoxelPos)
{
    if (!m_networkingEnabled || !m_networkManager)
    {
        return;
    }

    auto server = m_networkManager->getServer();
    if (!server)
    {
        return;
    }

    // Create spawn message
    FluidParticleSpawnMessage msg;
    msg.entityID = entityID;
    msg.islandID = islandID;
    msg.worldPosition = worldPos;
    msg.velocity = velocity;
    msg.originalVoxelPos = originalVoxelPos;

    // Broadcast to all clients
    server->broadcastToAllClients(&msg, sizeof(msg));
}

void GameServer::broadcastFluidParticleDespawn(EntityID entityID, uint32_t islandID, const Vec3& settledVoxelPos, 
                                               bool shouldCreateVoxel)
{
    if (!m_networkingEnabled || !m_networkManager)
    {
        return;
    }

    auto server = m_networkManager->getServer();
    if (!server)
    {
        return;
    }

    // Create despawn message
    FluidParticleDespawnMessage msg;
    msg.entityID = entityID;
    msg.islandID = islandID;
    msg.settledVoxelPos = settledVoxelPos;
    msg.shouldCreateVoxel = shouldCreateVoxel ? 1 : 0;

    // Broadcast to all clients
    server->broadcastToAllClients(&msg, sizeof(msg));
}

