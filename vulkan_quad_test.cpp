// vulkan_quad_test.cpp - Phase 3 Vulkan deferred renderer test
#include "engine/Rendering/Vulkan/VulkanContext.h"
#include "engine/Rendering/Vulkan/VulkanDeferred.h"
#include "engine/Rendering/Vulkan/VulkanQuadRenderer.h"
#include "engine/World/VoxelChunk.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <chrono>
#include <memory>

int main() {
    std::cout << "\n==============================================\n";
    std::cout << "  Vulkan Phase 3 Test - Deferred Rendering\n";
    std::cout << "==============================================\n\n";

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "❌ Failed to initialize GLFW\n";
        return 1;
    }
    
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Vulkan Deferred Renderer Test - Phase 3", nullptr, nullptr);
    if (!window) {
        std::cerr << "❌ Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }
    
    // Initialize Vulkan
    VulkanContext context;
    if (!context.init(window, true)) {
        std::cerr << "❌ Failed to initialize Vulkan\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::cout << "✅ Vulkan context initialized\n";
    
    // Initialize deferred renderer
    VulkanDeferred deferredRenderer;
    if (!deferredRenderer.initialize(context.getDevice(), context.getAllocator(),
                                     context.getSwapchainFormat(), 1280, 720)) {
        std::cerr << "❌ Failed to initialize deferred renderer\n";
        context.cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::cout << "✅ VulkanDeferred initialized (G-buffer + lighting pass)\n";
    
    // Initialize quad renderer (for geometry pass)
    VulkanQuadRenderer quadRenderer;
    if (!quadRenderer.initialize(&context)) {
        std::cerr << "❌ Failed to initialize quad renderer\n";
        deferredRenderer.destroy();
        context.cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::cout << "✅ VulkanQuadRenderer initialized\n";
    
    // Create test voxel chunk with simple quads
    std::cout << "\n📦 Creating test voxel chunk...\n";
    
    auto testChunk = std::make_unique<VoxelChunk>();
    testChunk->setIsClient(true);
    
    // Create render mesh
    auto mesh = std::make_shared<VoxelMesh>();
    
    // Quad 1: XY plane (front face) - Red-ish
    QuadFace quad1;
    quad1.position = glm::vec3(0.0f, 0.0f, 0.0f);
    quad1.width = 1.0f;
    quad1.height = 1.0f;
    // Pack normal (0, 0, 1) into 10:10:10:2 format
    uint32_t nx = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    uint32_t ny = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    uint32_t nz = static_cast<uint32_t>((1.0f * 0.5f + 0.5f) * 1023.0f);
    quad1.packedNormal = nx | (ny << 10) | (nz << 20);
    quad1.blockType = 1;  // Stone
    quad1.faceDir = 0;
    quad1.islandID = 0;
    mesh->quads.push_back(quad1);
    
    // Quad 2: XY plane offset - Green-ish
    QuadFace quad2;
    quad2.position = glm::vec3(2.0f, 0.0f, 0.0f);
    quad2.width = 1.0f;
    quad2.height = 1.0f;
    // Pack normal (0, 0, 1)
    nx = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    ny = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    nz = static_cast<uint32_t>((1.0f * 0.5f + 0.5f) * 1023.0f);
    quad2.packedNormal = nx | (ny << 10) | (nz << 20);
    quad2.blockType = 2;  // Dirt
    quad2.faceDir = 0;
    quad2.islandID = 0;
    mesh->quads.push_back(quad2);
    
    // Quad 3: YZ plane (side face) - Blue-ish
    QuadFace quad3;
    quad3.position = glm::vec3(1.0f, 1.0f, 0.0f);
    quad3.width = 1.0f;
    quad3.height = 1.0f;
    // Pack normal (1, 0, 0)
    nx = static_cast<uint32_t>((1.0f * 0.5f + 0.5f) * 1023.0f);
    ny = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    nz = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    quad3.packedNormal = nx | (ny << 10) | (nz << 20);
    quad3.blockType = 3;  // Grass
    quad3.faceDir = 1;
    quad3.islandID = 0;
    mesh->quads.push_back(quad3);
    
    // Quad 4: XZ plane (top face) - Yellow-ish
    QuadFace quad4;
    quad4.position = glm::vec3(0.0f, 2.0f, 0.0f);
    quad4.width = 1.0f;
    quad4.height = 1.0f;
    // Pack normal (0, 1, 0)
    nx = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    ny = static_cast<uint32_t>((1.0f * 0.5f + 0.5f) * 1023.0f);
    nz = static_cast<uint32_t>((0.0f * 0.5f + 0.5f) * 1023.0f);
    quad4.packedNormal = nx | (ny << 10) | (nz << 20);
    quad4.blockType = 4;  // Sand
    quad4.faceDir = 2;
    quad4.islandID = 0;
    mesh->quads.push_back(quad4);
    
    testChunk->setRenderMesh(mesh);
    
    std::cout << "  Created " << mesh->quads.size() << " test quads\n";
    
    // Register chunk with island ID 0
    uint32_t islandID = 0;
    glm::vec3 chunkOffset = glm::vec3(0.0f, 0.0f, 0.0f);
    
    // Set island transform for rendering
    quadRenderer.updateIslandTransform(islandID, glm::mat4(1.0f));
    
    std::cout << "  Registering chunk...\n";
    quadRenderer.registerChunk(testChunk.get(), islandID, chunkOffset);
    
    std::cout << "  Uploading mesh...\n";
    quadRenderer.uploadChunkMesh(testChunk.get());
    std::cout << "  Upload complete\n";
    
    std::cout << "✅ Test chunk registered and uploaded\n\n";
    std::cout << "🎮 Controls:\n";
    std::cout << "  ESC - Exit\n";
    std::cout << "  Watch for deferred lighting (ambient + sun lighting)!\n\n";
    
    // Camera setup
    glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 5.0f);
    glm::vec3 cameraTarget = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
    
    // Lighting setup
    VulkanDeferred::LightingParams lightingParams = {};
    lightingParams.sunDirection = glm::vec4(glm::normalize(glm::vec3(0.3f, -1.0f, 0.5f)), 0.8f); // direction + intensity
    lightingParams.moonDirection = glm::vec4(glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), 0.1f);
    lightingParams.sunColor = glm::vec4(1.0f, 0.95f, 0.8f, 1.0f);  // Warm sunlight
    lightingParams.moonColor = glm::vec4(0.3f, 0.4f, 0.6f, 1.0f);  // Cool moonlight
    lightingParams.ambientColor = glm::vec4(0.3f, 0.4f, 0.5f, 0.2f); // Subtle ambient
    lightingParams.cameraPos = glm::vec4(cameraPos, 1.0f);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float>(currentTime - startTime).count();
        
        // Rotate camera around scene
        float radius = 5.0f;
        cameraPos.x = radius * sin(time * 0.5f);
        cameraPos.z = radius * cos(time * 0.5f);
        
        // Create view and projection matrices
        glm::mat4 view = glm::lookAt(cameraPos, cameraTarget, cameraUp);
        glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 100.0f);
        projection[1][1] *= -1; // Flip Y for Vulkan
        glm::mat4 viewProjection = projection * view;
        
        // Update lighting params with camera position
        lightingParams.cameraPos = glm::vec4(cameraPos, 1.0f);
        
        // Update dynamic buffers BEFORE render pass
        VkCommandBuffer updateCmd = context.beginSingleTimeCommands();
        quadRenderer.updateDynamicBuffers(updateCmd, viewProjection);
        context.endSingleTimeCommands(updateCmd);
        
        // Begin frame WITHOUT starting default render pass (we use custom deferred pipeline)
        uint32_t imageIndex;
        if (!context.beginFrame(imageIndex, false)) {
            continue;
        }
        
        VkCommandBuffer cmd = context.getCurrentCommandBuffer();
        
        try {
            // PHASE 3: Two-pass deferred rendering
            
            // Pass 1: Geometry pass (render quads to G-buffer)
            deferredRenderer.beginGeometryPass(cmd);
            // TODO: Render quads with geometry pipeline (needs integration)
            // For now, just end the pass to test the pipeline
            deferredRenderer.endGeometryPass(cmd);
            
            // Pass 2: Lighting pass (fullscreen quad reading G-buffer)
            // This renders directly to the swapchain image
            deferredRenderer.renderLightingPass(cmd, context.getSwapchainImageView(imageIndex), 
                                                lightingParams);
            
        } catch (const std::exception& e) {
            std::cerr << "❌ Render error: " << e.what() << "\n";
            break;
        }
        
        // End frame WITHOUT calling endRenderPass (we already ended it in lighting pass)
        context.endFrame(imageIndex, false);
    }
    
    // Wait for GPU
    vkDeviceWaitIdle(context.getDevice());
    
    // Cleanup
    std::cout << "\n🧹 Cleaning up...\n";
    deferredRenderer.destroy();
    quadRenderer.shutdown();
    context.cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\n✅ Vulkan Phase 3 test complete!\n";
    std::cout << "Next: Phase 4 - Shadow cascades\n\n";
    
    return 0;
}
