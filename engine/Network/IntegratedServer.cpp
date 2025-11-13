#include "IntegratedServer.h"

#include <cstring>

#include "VoxelCompression.h"
#include "pch.h"
#include "../Profiling/Profiler.h"

IntegratedServer::IntegratedServer() : host(nullptr), nextSequenceNumber(0) {}

IntegratedServer::~IntegratedServer()
{
    stopServer();
}

bool IntegratedServer::startServer(uint16_t port)
{
    if (host)
    {
        std::cout << "Server already running!" << std::endl;
        return false;
    }

    std::cout << "Creating ENet server on port " << port << "..." << std::endl;

    ENetAddress address;
    // Explicitly bind to localhost instead of ENET_HOST_ANY
    enet_address_set_host(&address, "127.0.0.1");
    address.port = port;

    // Removed verbose debug output

    // Create server host (allow up to 32 clients)
    host = enet_host_create(&address, 32, 2, 0, 0);
    if (!host)
    {
        std::cout << "Failed to create ENet server host!" << std::endl;
        std::cout << "This might be a permissions issue or port conflict" << std::endl;
        return false;
    }

    // Removed verbose debug output
    return true;
}

void IntegratedServer::stopServer()
{
    if (host)
    {
        enet_host_destroy(host);
        host = nullptr;
        connectedClients.clear();
        std::cout << "Server stopped" << std::endl;
    }
}

void IntegratedServer::update()
{
    PROFILE_SCOPE("IntegratedServer::update");
    
    if (!host)
        return;

    ENetEvent event;

    // Process all pending events (non-blocking)
    while (enet_host_service(host, &event, 0) > 0)
    {
        handleClientEvent(event);
    }
}

void IntegratedServer::handleClientEvent(const ENetEvent& event)
{
    switch (event.type)
    {
        case ENET_EVENT_TYPE_CONNECT:
        {
            // Removed verbose debug output

            connectedClients.push_back(event.peer);

            if (onClientConnected)
            {
                onClientConnected(event.peer);
            }
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        {
            std::cout << "Client disconnected" << std::endl;

            // Remove from connected clients list
            auto it = std::find(connectedClients.begin(), connectedClients.end(), event.peer);
            if (it != connectedClients.end())
            {
                connectedClients.erase(it);
            }

            if (onClientDisconnected)
            {
                onClientDisconnected(event.peer);
            }
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE:
        {
            processClientMessage(event.peer, event.packet);
            enet_packet_destroy(event.packet);
            break;
        }

        default:
            break;
    }
}

void IntegratedServer::processClientMessage(ENetPeer* client, ENetPacket* packet)
{
    if (packet->dataLength < sizeof(NetworkMessageType))
        return;

    NetworkMessageType messageType = *(NetworkMessageType*) packet->data;

    switch (messageType)
    {
        case NetworkMessageType::PLAYER_MOVEMENT_REQUEST:
        {
            if (packet->dataLength >= sizeof(PlayerMovementRequest))
            {
                PlayerMovementRequest request = *(PlayerMovementRequest*) packet->data;

                if (onPlayerMovementRequest)
                {
                    onPlayerMovementRequest(client, request);
                }
            }
            break;
        }

        case NetworkMessageType::VOXEL_CHANGE_REQUEST:
        {
            if (packet->dataLength >= sizeof(VoxelChangeRequest))
            {
                VoxelChangeRequest request = *(VoxelChangeRequest*) packet->data;

                if (onVoxelChangeRequest)
                {
                    onVoxelChangeRequest(client, request);
                }
            }
            break;
        }

        case NetworkMessageType::PILOTING_INPUT:
        {
            if (packet->dataLength >= sizeof(PilotingInputMessage))
            {
                PilotingInputMessage input = *(PilotingInputMessage*) packet->data;

                if (onPilotingInput)
                {
                    onPilotingInput(client, input);
                }
            }
            break;
        }

        default:
            std::cout << "Unknown message type from client: " << (int) messageType << std::endl;
            break;
    }
}

void IntegratedServer::broadcastHelloWorld()
{
    HelloWorldMessage msg;
    // Use a bounded copy to avoid unsafe strcpy usage and ensure null-termination
    std::strncpy(msg.message, "Hello from server!", sizeof(msg.message) - 1);
    msg.message[sizeof(msg.message) - 1] = '\0';

    broadcastToAllClients(&msg, sizeof(msg));
}

void IntegratedServer::broadcastPlayerPosition(uint32_t playerId, const Vec3& position,
                                               const Vec3& velocity)
{
    PlayerPositionUpdate update;
    update.playerId = playerId;
    update.position = position;
    update.velocity = velocity;

    broadcastToAllClients(&update, sizeof(update));
}

void IntegratedServer::sendWorldStateToClient(ENetPeer* client, const WorldStateMessage& worldState)
{
    // Removed verbose debug output
    sendToClient(client, &worldState, sizeof(worldState));
}

void IntegratedServer::sendCompressedIslandToClient(ENetPeer* client, uint32_t islandID,
                                                    const Vec3& position, const uint8_t* voxelData,
                                                    uint32_t voxelDataSize)
{
    if (!client || !voxelData || voxelDataSize == 0)
    {
        std::cerr << "Invalid parameters for compressed island transmission" << std::endl;
        return;
    }

    // Compress the voxel data
    std::vector<uint8_t> compressedData;
    uint32_t compressedSize =
        VoxelCompression::compressRLE(voxelData, voxelDataSize, compressedData);

    if (compressedSize == 0 || compressedData.empty())
    {
        std::cerr << "Failed to compress island data for island " << islandID << std::endl;
        return;
    }

    // Create header
    CompressedIslandHeader header;
    header.islandID = islandID;
    header.position = position;
    header.originalSize = voxelDataSize;
    header.compressedSize = compressedSize;

    // Create combined packet: header + compressed data
    std::vector<uint8_t> combinedPacket(sizeof(header) + compressedSize);

    // Copy header to the beginning
    std::memcpy(combinedPacket.data(), &header, sizeof(header));

    // Copy compressed data after header
    std::memcpy(combinedPacket.data() + sizeof(header), compressedData.data(), compressedSize);

    // Send as single packet
    sendToClient(client, combinedPacket.data(), combinedPacket.size());
}

// NEW: Send individual chunk with coordinates for multi-chunk islands
void IntegratedServer::sendCompressedChunkToClient(ENetPeer* client, uint32_t islandID, const Vec3& chunkCoord, const Vec3& islandPosition, const uint8_t* voxelData, uint32_t voxelDataSize)
{
    if (!client || !voxelData || voxelDataSize == 0)
    {
        std::cerr << "Invalid parameters for compressed chunk transmission" << std::endl;
        return;
    }

    // Compress the voxel data using LZ4
    std::vector<uint8_t> compressedData;
    uint32_t compressedSize = VoxelCompression::compressLZ4(voxelData, voxelDataSize, compressedData);

    if (compressedSize == 0 || compressedData.empty())
    {
        std::cerr << "Failed to compress chunk data for island " << islandID 
                  << " chunk (" << chunkCoord.x << "," << chunkCoord.y << "," << chunkCoord.z << ")" << std::endl;
        return;
    }

    // Create header
    CompressedChunkHeader header;
    header.islandID = islandID;
    header.chunkCoord = chunkCoord;
    header.islandPosition = islandPosition;
    header.originalSize = voxelDataSize;
    header.compressedSize = compressedSize;

    // Create combined packet: header + compressed data
    std::vector<uint8_t> combinedPacket(sizeof(header) + compressedSize);

    // Copy header to the beginning
    std::memcpy(combinedPacket.data(), &header, sizeof(header));

    // Copy compressed data after header
    std::memcpy(combinedPacket.data() + sizeof(header), compressedData.data(), compressedSize);

    // Send as single packet
    sendToClient(client, combinedPacket.data(), combinedPacket.size());
}

void IntegratedServer::sendToClient(ENetPeer* client, const void* data, size_t size)
{
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, packet);
}

void IntegratedServer::broadcastToAllClients(const void* data, size_t size)
{
    if (!host || connectedClients.empty())
        return;

    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);

    for (ENetPeer* client : connectedClients)
    {
        enet_peer_send(client, 0, packet);
    }
}

void IntegratedServer::broadcastVoxelChange(uint32_t islandID, const Vec3& localPos,
                                            uint8_t voxelType, uint32_t authorPlayerId)
{
    VoxelChangeUpdate update;
    update.sequenceNumber = nextSequenceNumber++;
    update.islandID = islandID;
    update.localPos = localPos;
    update.voxelType = voxelType;
    update.authorPlayerId = authorPlayerId;

    broadcastToAllClients(&update, sizeof(update));
}

void IntegratedServer::broadcastEntityState(const EntityStateUpdate& entityState)
{
    broadcastToAllClients(&entityState, sizeof(entityState));
}
