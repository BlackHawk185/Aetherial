# Add all engine source files (including new systems)
add_library(engine STATIC
    # Core systems (new modular architecture)
    Core/SimulationState.cpp
    Core/ServerWorld.cpp
    Core/ClientWorld.cpp
    Core/GameServer.cpp
    Core/GameClient.cpp
    Core/Window.cpp
    Profiling/Profiler.cpp
    Profiling/DebugDiagnostics.cpp
    
    # World systems
    World/VoxelChunk.cpp
    World/IslandChunkSystem.cpp
    World/VoxelRaycaster.cpp
    World/BlockType.cpp
    World/BlockDamageTracker.cpp
    World/ConnectivityAnalyzer.cpp
    World/ElementRecipes.cpp
    World/VoronoiIslandPlacer.cpp
    World/BiomeSystem.cpp
    World/TreeGenerator.cpp
    World/FluidSystem.cpp
    
    # Culling systems
    Culling/Frustum.cpp
    
    # Rendering systems
    Rendering/TextureManager.cpp
    Assets/GLBLoader.cpp
    
    # Vulkan rendering systems
    Rendering/Vulkan/VulkanMemoryAllocator.cpp
    Rendering/Vulkan/VulkanContext.cpp
    Rendering/Vulkan/VulkanTriangleRenderer.cpp
    Rendering/Vulkan/VulkanQuadRenderer.cpp
    Rendering/Vulkan/VulkanModelRenderer.cpp
    Rendering/Vulkan/VulkanBlockHighlighter.cpp
    Rendering/Vulkan/VulkanImage.cpp
    Rendering/Vulkan/VulkanGBuffer.cpp
    Rendering/Vulkan/VulkanDeferred.cpp
    Rendering/Vulkan/VulkanShadowMap.cpp
    Rendering/Vulkan/VulkanSSPR.cpp
    Rendering/Vulkan/VulkanSkyRenderer.cpp
    Rendering/Vulkan/VulkanCloudRenderer.cpp
    
    # Input systems
    Input/Camera.cpp
    Input/PlayerController.cpp
    
    # UI systems
    UI/HUD.cpp
    UI/PeriodicTableUI.cpp
    
    # Math systems
    Math/Vec3.cpp
    
    # ECS systems
    ECS/ECS.cpp
    
    # Physics systems
    Physics/PhysicsSystem.cpp
    
    # Time systems
    Time/TimeManager.cpp
    Time/TimeEffects.cpp
    Time/DayNightController.cpp
    
    # Network systems - Re-enabled with proper ENet integration
    Network/NetworkManager.cpp
    Network/IntegratedServer.cpp
    Network/NetworkClient.cpp
    Network/VoxelCompression.cpp
)

# === Third-party sources: ImGui ===
file(GLOB IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/libs/imgui/*.cpp
)

# Add ImGui backend implementations
set(IMGUI_BACKEND_SRC
    ${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_vulkan.cpp
)

# Add third-party sources to engine target
target_sources(engine PRIVATE
    ${IMGUI_SRC}
    ${IMGUI_BACKEND_SRC}
)

# Enable precompiled headers (PCH)
target_precompile_headers(engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

# Engine includes
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/imgui
    ${CMAKE_SOURCE_DIR}/libs/imgui/backends
    ${CMAKE_SOURCE_DIR}/libs/tinygltf
    ${CMAKE_SOURCE_DIR}/libs/stb
    ${CMAKE_SOURCE_DIR}/libs/bimg/3rdparty/stb
    ${CMAKE_SOURCE_DIR}/libs/FastNoiseLite
    ${CMAKE_SOURCE_DIR}/libs/FastNoiseSIMD
)

target_compile_features(engine PUBLIC cxx_std_17)

# Set include directories for third-party libraries
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}  # For internal includes
    ${CMAKE_SOURCE_DIR}/libs/enet/include  # For ENet headers
)

# MSVC compatibility
if(MSVC)
    # Ensure correct __cplusplus value
    target_compile_options(engine PRIVATE /Zc:__cplusplus)
    # Allow parallel compilation and avoid PDB write conflicts by enabling /MP and /FS
    # /MP enables multi-processor compilation; /FS serializes access to PDB to avoid races
    target_compile_options(engine PRIVATE /MP /FS)
endif()

find_package(OpenGL REQUIRED)

# Find Vulkan (optional for now - migration in progress)
find_package(Vulkan)

# Fetch vk-bootstrap for Vulkan initialization
include(FetchContent)
FetchContent_Declare(
    vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
    GIT_TAG v1.3.295
)

# Fetch VulkanMemoryAllocator for memory management
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG v3.3.0
)

FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator)

# Include auto-generated shader paths header
target_include_directories(engine PRIVATE ${CMAKE_BINARY_DIR}/engine)

# Link third-party libraries
target_link_libraries(engine PUBLIC
    glfw
    enet
    lz4
    stb
    glm::glm
    FastNoiseSIMD
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    VulkanMemoryAllocator
)

# Ensure GLFW does not include OpenGL headers
target_compile_definitions(engine PUBLIC GLFW_INCLUDE_NONE)
