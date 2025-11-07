#pragma once
#include <enet/enet.h>
#include "NetworkMessages.h"
#include <functional>
#include <string>
#include <cstdint>

/**
 * @brief Simple client component for connecting to remote servers
 * Always present, but only used when joining someone else's game
 */
class NetworkClient {
private:
    ENetHost* client;
    ENetPeer* serverConnection;
    uint32_t nextSequenceNumber;
    
public:
    NetworkClient();
    ~NetworkClient();
    
    // Connection management
    bool connectToServer(const std::string& host, uint16_t port = 7777);
    void disconnect();
    bool isConnected() const { return serverConnection != nullptr; }
    
    // Called each frame to handle networking
    void update();
    
    // Send messages to server
    void sendMovementRequest(const Vec3& intendedPosition, const Vec3& velocity, float deltaTime);
    uint32_t sendVoxelChangeRequest(uint32_t islandID, const Vec3& localPos, uint8_t voxelType);  // Returns sequence number
    void sendPilotingInput(uint32_t islandID, float thrustY, float rotationYaw);
    void sendToServer(const void* data, size_t size);
    
    // Callbacks for server messages
    std::function<void()> onConnectedToServer;
    std::function<void()> onDisconnectedFromServer;
    std::function<void(const PlayerPositionUpdate&)> onPlayerPositionUpdate;
    std::function<void(const HelloWorldMessage&)> onHelloWorld;
    std::function<void(const WorldStateMessage&)> onWorldStateReceived;
    std::function<void(uint32_t, const Vec3&, const uint8_t*, uint32_t)> onCompressedIslandReceived;
    
    // NEW: Callback for individual chunk data with coordinates
    std::function<void(uint32_t, const Vec3&, const Vec3&, const uint8_t*, uint32_t)> onCompressedChunkReceived;
    
    std::function<void(const VoxelChangeUpdate&)> onVoxelChangeReceived;
    std::function<void(const EntityStateUpdate&)> onEntityStateUpdate;
    
private:
    void handleServerEvent(const ENetEvent& event);
    void processServerMessage(ENetPacket* packet);
};
