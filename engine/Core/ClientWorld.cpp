// ClientWorld.cpp - Client-side world implementation
#include "pch.h"
#include "ClientWorld.h"
#include <iostream>

ClientWorld::ClientWorld()
{
}

ClientWorld::~ClientWorld()
{
    shutdown();
}

bool ClientWorld::initialize(bool createDefaultWorld)
{
    if (m_initialized)
    {
        std::cerr << "ClientWorld already initialized!" << std::endl;
        return false;
    }

    std::cout << "[CLIENT] Initializing ClientWorld..." << std::endl;

    // Initialize base simulation
    if (!m_simulation.initialize(createDefaultWorld))
    {
        std::cerr << "[CLIENT] Failed to initialize simulation state!" << std::endl;
        return false;
    }

    // CLIENT: Mark island system as client-side (enables mesh operations)
    m_simulation.getIslandSystem()->setIsClient(true);
    std::cout << "[CLIENT] Island system marked as client-side (mesh generation enabled)" << std::endl;

    m_initialized = true;
    return true;
}

void ClientWorld::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::cout << "[CLIENT] Shutting down ClientWorld..." << std::endl;
    m_simulation.shutdown();
    m_pendingVoxelChanges.clear();
    m_initialized = false;
}

void ClientWorld::update(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    // Update base simulation (physics, islands)
    // CLIENT does NOT run fluid simulation - receives updates from server
    m_simulation.updateSimulation(deltaTime);
}

uint32_t ClientWorld::applyPredictedVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType, uint32_t sequenceNumber)
{
    if (!m_initialized)
    {
        return 0;
    }

    // Get current voxel type for rollback
    uint8_t previousType = m_simulation.getIslandSystem()->getVoxelFromIsland(islandID, localPos);
    
    // Apply prediction immediately (uses mesh generation for responsive feel)
    m_simulation.getIslandSystem()->setVoxelWithMesh(islandID, localPos, voxelType);
    
    // Track this prediction for reconciliation
    m_pendingVoxelChanges[sequenceNumber] = {
        islandID,
        localPos,
        voxelType,
        previousType
    };
    
    std::cout << "[CLIENT] Predicted voxel change (seq " << sequenceNumber << "): island " 
              << islandID << " pos (" << localPos.x << ", " << localPos.y << ", " << localPos.z 
              << ") type=" << (int)voxelType << std::endl;
    
    return sequenceNumber;
}

void ClientWorld::reconcileVoxelChange(uint32_t sequenceNumber, uint32_t islandID, const Vec3& localPos, uint8_t voxelType)
{
    if (!m_initialized)
    {
        return;
    }

    // Check if this is a confirmation of our prediction
    auto it = m_pendingVoxelChanges.find(sequenceNumber);
    if (it != m_pendingVoxelChanges.end())
    {
        const PendingVoxelChange& prediction = it->second;
        
        // Check if server's result matches our prediction
        if (prediction.islandID == islandID &&
            prediction.localPos == localPos &&
            prediction.predictedType == voxelType)
        {
            // Server confirmed our prediction - no action needed
            std::cout << "[CLIENT] Server confirmed prediction (seq " << sequenceNumber << ")" << std::endl;
        }
        else
        {
            // Server rejected or modified our prediction - apply correction
            std::cout << "[CLIENT] Server corrected prediction (seq " << sequenceNumber 
                      << ") - applying server's version" << std::endl;
            m_simulation.getIslandSystem()->setVoxelWithMesh(islandID, localPos, voxelType);
        }
        
        // Remove from pending predictions
        m_pendingVoxelChanges.erase(it);
    }
    else
    {
        // This is a change from another player or server-initiated - apply it
        std::cout << "[CLIENT] Applying non-predicted voxel change from server" << std::endl;
        m_simulation.getIslandSystem()->setVoxelWithMesh(islandID, localPos, voxelType);
    }
}

void ClientWorld::applyServerVoxelChange(uint32_t islandID, const Vec3& localPos, uint8_t voxelType)
{
    if (!m_initialized)
    {
        return;
    }

    // Direct server update (not a prediction reconciliation)
    m_simulation.getIslandSystem()->setVoxelWithMesh(islandID, localPos, voxelType);
    
    std::cout << "[CLIENT] Applied server voxel change: island " << islandID 
              << " pos (" << localPos.x << ", " << localPos.y << ", " << localPos.z 
              << ") type=" << (int)voxelType << std::endl;
}
