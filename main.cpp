// main.cpp - Modular MMORPG Engine with Unified Networking Architecture
// This demonstrates the unified networking architecture where:
// - GameServer runs the authoritative simulation (can be headless)
// - GameClient ALWAYS connects via network layer (even for local games)
// - Single networking code path for consistent debugging and development
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

void printHelp()
{
    std::cout << "🎮 MMORPG Engine Prototype - Usage:" << std::endl;
    std::cout << "  Default (no args):     Integrated mode - local server + networked client"
              << std::endl;
    std::cout << "  --enable-networking:   Integrated mode + allow external connections"
              << std::endl;
    std::cout << "  --server:              Server-only mode (headless)" << std::endl;
    std::cout << "  --client <address>:    Connect to remote server" << std::endl;
    std::cout << "  --debug:               Enable OpenGL debug output" << std::endl;
    std::cout << "  --help:                Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "💡 All modes now use unified networking for consistent debugging" << std::endl;
}

#include "engine/Core/GameClient.h"
#include "engine/Core/GameServer.h"
#include "engine/Time/TimeEffects.h"
#include "engine/Time/TimeManager.h"
#include "engine/Profiling/DebugDiagnostics.h"
#include "engine/Profiling/Profiler.h"

// Global systems (external declarations - defined in engine library)
extern TimeManager* g_timeManager;
extern TimeEffects* g_timeEffects;

enum class RunMode
{
    INTEGRATED,   // Run both server and client in same process (unified networking)
    SERVER_ONLY,  // Run headless server only (for dedicated servers)
    CLIENT_ONLY   // Connect to remote server
};

int main(int argc, char* argv[])
{
    // Install debug diagnostics early (only meaningful on MSVC Debug builds)
    DebugDiagnostics::Install();

    // Check for help first (before any initialization)
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printHelp();
            return 0;
        }
    }

    // Parse command line arguments
    RunMode runMode = RunMode::INTEGRATED;  // Default to working integrated mode
    std::string serverAddress = "localhost";
    uint16_t serverPort = 12346;    // Changed from 7777 to a higher port number
    bool enableNetworking = false;  // Allow external connections in integrated mode
    bool enableDebug = false;       // Enable OpenGL debug output

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--server") == 0 || strcmp(argv[i], "SERVER_ONLY") == 0)
        {
            runMode = RunMode::SERVER_ONLY;
        }
        else if (strcmp(argv[i], "--enable-networking") == 0)
        {
            enableNetworking = true;  // Allow external connections to integrated mode
        }
        else if (strcmp(argv[i], "--debug") == 0)
        {
            enableDebug = true;  // Enable OpenGL debug output
        }
        else if (strcmp(argv[i], "--client") == 0 && i + 1 < argc)
        {
            runMode = RunMode::CLIENT_ONLY;
            serverAddress = argv[i + 1];
            i++;  // Skip next argument

            // Optional port argument
            if (i + 1 < argc && isdigit(argv[i + 1][0]))
            {
                serverPort = static_cast<uint16_t>(atoi(argv[i + 1]));
                i++;
            }
        }
        else if (strcmp(argv[i], "CLIENT_ONLY") == 0)
        {
            runMode = RunMode::CLIENT_ONLY;
            // Use default localhost
        }
    }

    // Enable profiler to diagnose performance bottlenecks
    g_profiler.setEnabled(false);  // Disabled - profiling complete

    g_timeManager = new TimeManager();
    g_timeEffects = new TimeEffects();

    switch (runMode)
    {
        case RunMode::INTEGRATED:
        {
            // Create and initialize server (ALWAYS with networking enabled for unified
            // architecture)
            GameServer server;
            if (!server.initialize(60.0f, true, serverPort))
            {  // Force networking ON
                std::cerr << "Failed to initialize game server!" << std::endl;
                return 1;
            }

            // Start server in background thread
            server.runAsync();

            // Give server a moment to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Create and initialize client
            GameClient client;
            if (!client.initialize(enableDebug))
            {
                std::cerr << "Failed to initialize game client!" << std::endl;
                server.stop();
                return 1;
            }

            // Connect client to server via NETWORK (unified path)
            if (!client.connectToRemoteServer("127.0.0.1", serverPort))
            {
                std::cerr << "Failed to connect client to local server!" << std::endl;
                server.stop();
                return 1;
            }

            // Main client loop
            auto lastTime = std::chrono::high_resolution_clock::now();
            while (true)
            {
                auto currentTime = std::chrono::high_resolution_clock::now();
                float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
                lastTime = currentTime;

                // Clamp delta time
                if (deltaTime > 0.05f)
                    deltaTime = 0.05f;

                // Update client
                if (!client.update(deltaTime))
                {
                    break;  // Client wants to exit
                }

                // Update time effects
                if (g_timeEffects)
                {
                    g_timeEffects->update(deltaTime);
                }
            }

            client.shutdown();
            server.stop();

            break;
        }

        case RunMode::SERVER_ONLY:
        {
            // Create and initialize server with networking enabled
            GameServer server;
            if (!server.initialize(60.0f, true, serverPort))
            {  // Enable networking
                std::cerr << "Failed to initialize game server!" << std::endl;
                return 1;
            }

            // Run server (blocks until stopped)
            server.run();

            break;
        }

        case RunMode::CLIENT_ONLY:
        {
            // Create and initialize client
            GameClient client;
            if (!client.initialize(enableDebug))
            {
                std::cerr << "Failed to initialize game client!" << std::endl;
                return 1;
            }

            // Connect to remote server
            if (!client.connectToRemoteServer(serverAddress, serverPort))
            {
                std::cerr << "Failed to connect to remote server!" << std::endl;
                return 1;
            }

            // Main client loop
            auto lastTime = std::chrono::high_resolution_clock::now();
            while (true)
            {
                auto currentTime = std::chrono::high_resolution_clock::now();
                float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
                lastTime = currentTime;

                // Clamp delta time
                if (deltaTime > 0.05f)
                    deltaTime = 0.05f;

                // Update client
                if (!client.update(deltaTime))
                {
                    break;  // Client wants to exit
                }

                // Update time effects
                if (g_timeEffects)
                {
                    g_timeEffects->update(deltaTime);
                }
            }

            // Removed verbose debug output
            client.shutdown();

            break;
        }
    }

    // Disable profiler early in teardown to avoid any late-use
    g_profiler.setEnabled(false);
    g_profiler.clearAll();

    delete g_timeEffects;
    delete g_timeManager;
    g_timeEffects = nullptr;
    g_timeManager = nullptr;

    return 0;
}
