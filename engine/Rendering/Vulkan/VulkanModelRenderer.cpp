// VulkanModelRenderer.cpp
#include "VulkanModelRenderer.h"
#include "VulkanContext.h"
#include "../../World/VoxelChunk.h"
#include "../../World/BlockType.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

VulkanModelRenderer::VulkanModelRenderer() = default;

VulkanModelRenderer::~VulkanModelRenderer() {
    shutdown();
}

bool VulkanModelRenderer::initialize(VulkanContext* ctx, VkRenderPass gbufferRenderPass) {
    if (!ctx) return false;
    
    m_context = ctx;
    m_allocator = ctx->getAllocator();
    m_gbufferRenderPass = gbufferRenderPass;
    
    // Create island transform buffer (64 islands * mat4)
    m_islandTransformBuffer = std::make_unique<VulkanBuffer>();
    if (!m_islandTransformBuffer->create(m_allocator, 64 * sizeof(glm::mat4), 
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VMA_MEMORY_USAGE_CPU_TO_GPU)) {
        std::cerr << "Failed to create island transform buffer" << std::endl;
        return false;
    }
    
    // Create instance buffer (64K instances initially)
    m_instanceBuffer = std::make_unique<VulkanBuffer>();
    if (!m_instanceBuffer->create(m_allocator, 65536 * sizeof(InstanceData),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VMA_MEMORY_USAGE_CPU_TO_GPU)) {
        std::cerr << "Failed to create instance buffer" << std::endl;
        return false;
    }
    
    createDescriptors();
    createPipeline();
    
    std::cout << "✓ VulkanModelRenderer initialized" << std::endl;
    return true;
}

void VulkanModelRenderer::shutdown() {
    if (!m_context) return;
    
    VkDevice device = m_context->getDevice();
    
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    
    m_models.clear();
    m_instanceBuffer.reset();
    m_islandTransformBuffer.reset();
    m_context = nullptr;
}

bool VulkanModelRenderer::loadModel(uint8_t blockID, const std::string& glbPath) {
    // Check if already loaded
    if (m_models.find(blockID) != m_models.end()) {
        return true;
    }
    
    GLBModelCPU cpuModel;
    if (!GLBLoader::loadGLB(glbPath, cpuModel) || !cpuModel.valid) {
        std::cerr << "⚠ Failed to load GLB: " << glbPath << " - creating magenta fallback" << std::endl;
        createMagentaCube(blockID);
        return true;  // Fallback created successfully
    }
    
    // Upload GLB data to GPU
    ModelData modelData;
    modelData.isFallback = false;
    
    // Combine all primitives into single buffers
    std::vector<float> allVertices;
    std::vector<uint32_t> allIndices;
    
    for (const auto& prim : cpuModel.primitives) {
        uint32_t baseVertex = allVertices.size() / 8;  // 8 floats per vertex
        
        allVertices.insert(allVertices.end(), prim.interleaved.begin(), prim.interleaved.end());
        
        for (uint32_t idx : prim.indices) {
            allIndices.push_back(baseVertex + idx);
        }
    }
    
    // Create vertex buffer
    size_t vertexDataSize = allVertices.size() * sizeof(float);
    modelData.vertexBuffer = std::make_unique<VulkanBuffer>();
    if (!modelData.vertexBuffer->create(m_allocator, vertexDataSize,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VMA_MEMORY_USAGE_GPU_ONLY)) {
        std::cerr << "Failed to create vertex buffer for model" << std::endl;
        createMagentaCube(blockID);
        return true;
    }
    
    // Create index buffer
    size_t indexDataSize = allIndices.size() * sizeof(uint32_t);
    modelData.indexBuffer = std::make_unique<VulkanBuffer>();
    if (!modelData.indexBuffer->create(m_allocator, indexDataSize,
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_GPU_ONLY)) {
        std::cerr << "Failed to create index buffer for model" << std::endl;
        createMagentaCube(blockID);
        return true;
    }
    
    // Upload data via staging buffer
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    VulkanBuffer vertexStaging;
    VulkanBuffer indexStaging;
    
    // Upload vertices
    vertexStaging.create(m_allocator, vertexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* vertexData = vertexStaging.map();
    memcpy(vertexData, allVertices.data(), vertexDataSize);
    vertexStaging.unmap();
    
    VkBufferCopy vertexCopy{};
    vertexCopy.size = vertexDataSize;
    vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), modelData.vertexBuffer->getBuffer(), 1, &vertexCopy);
    
    // Upload indices
    indexStaging.create(m_allocator, indexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* indexData = indexStaging.map();
    memcpy(indexData, allIndices.data(), indexDataSize);
    indexStaging.unmap();
    
    VkBufferCopy indexCopy{};
    indexCopy.size = indexDataSize;
    vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), modelData.indexBuffer->getBuffer(), 1, &indexCopy);
    
    m_context->endSingleTimeCommands(cmd);
    
    // Now safe to destroy staging buffers
    vertexStaging.destroy();
    indexStaging.destroy();
    
    modelData.indexCount = allIndices.size();
    m_models[blockID] = std::move(modelData);
    
    std::cout << "✓ Loaded GLB model for BlockID " << (int)blockID << ": " 
              << allVertices.size()/8 << " verts, " << allIndices.size() << " indices" << std::endl;
    
    return true;
}

void VulkanModelRenderer::createMagentaCube(uint8_t blockID) {
    // Magenta 1x1x1 cube vertices (pos + normal + uv = 8 floats per vertex)
    // 24 vertices (4 per face, 6 faces) for proper normals
    float cubeVertices[] = {
        // -X face (left)
        0.0f, 0.0f, 0.0f,  -1.0f, 0.0f, 0.0f,  0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,  0.0f, 1.0f,
        0.0f, 1.0f, 1.0f,  -1.0f, 0.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 0.0f,  1.0f, 0.0f,
        
        // +X face (right)
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,   1.0f, 0.0f, 0.0f,  0.0f, 1.0f,
        1.0f, 1.0f, 1.0f,   1.0f, 0.0f, 0.0f,  1.0f, 1.0f,
        1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f,  1.0f, 0.0f,
        
        // -Y face (bottom)
        0.0f, 0.0f, 0.0f,   0.0f, -1.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   0.0f, -1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 1.0f,   0.0f, -1.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 0.0f, 1.0f,   0.0f, -1.0f, 0.0f,  0.0f, 1.0f,
        
        // +Y face (top)
        0.0f, 1.0f, 0.0f,   0.0f,  1.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,   0.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f, 1.0f,   0.0f,  1.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 1.0f, 1.0f,   0.0f,  1.0f, 0.0f,  0.0f, 1.0f,
        
        // -Z face (back)
        0.0f, 0.0f, 0.0f,   0.0f, 0.0f, -1.0f,  0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   0.0f, 0.0f, -1.0f,  1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,   0.0f, 0.0f, -1.0f,  1.0f, 1.0f,
        0.0f, 1.0f, 0.0f,   0.0f, 0.0f, -1.0f,  0.0f, 1.0f,
        
        // +Z face (front)
        0.0f, 0.0f, 1.0f,   0.0f, 0.0f,  1.0f,  0.0f, 0.0f,
        1.0f, 0.0f, 1.0f,   0.0f, 0.0f,  1.0f,  1.0f, 0.0f,
        1.0f, 1.0f, 1.0f,   0.0f, 0.0f,  1.0f,  1.0f, 1.0f,
        0.0f, 1.0f, 1.0f,   0.0f, 0.0f,  1.0f,  0.0f, 1.0f,
    };
    
    uint32_t cubeIndices[] = {
        0,1,2, 0,2,3,       // -X
        4,5,6, 4,6,7,       // +X
        8,9,10, 8,10,11,    // -Y
        12,13,14, 12,14,15, // +Y
        16,17,18, 16,18,19, // -Z
        20,21,22, 20,22,23  // +Z
    };
    
    ModelData modelData;
    modelData.isFallback = true;
    modelData.indexCount = 36;
    
    // Create vertex buffer
    size_t vertexDataSize = sizeof(cubeVertices);
    modelData.vertexBuffer = std::make_unique<VulkanBuffer>();
    modelData.vertexBuffer->create(m_allocator, vertexDataSize,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY);
    
    // Create index buffer
    size_t indexDataSize = sizeof(cubeIndices);
    modelData.indexBuffer = std::make_unique<VulkanBuffer>();
    modelData.indexBuffer->create(m_allocator, indexDataSize,
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY);
    
    // Upload via staging
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    VulkanBuffer vertexStaging;
    VulkanBuffer indexStaging;
    
    vertexStaging.create(m_allocator, vertexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* vertexData = vertexStaging.map();
    memcpy(vertexData, cubeVertices, vertexDataSize);
    vertexStaging.unmap();
    
    VkBufferCopy vertexCopy{};
    vertexCopy.size = vertexDataSize;
    vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), modelData.vertexBuffer->getBuffer(), 1, &vertexCopy);
    
    indexStaging.create(m_allocator, indexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* indexData = indexStaging.map();
    memcpy(indexData, cubeIndices, indexDataSize);
    indexStaging.unmap();
    
    VkBufferCopy indexCopy{};
    indexCopy.size = indexDataSize;
    vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), modelData.indexBuffer->getBuffer(), 1, &indexCopy);
    
    m_context->endSingleTimeCommands(cmd);
    
    // Now safe to destroy staging buffers
    vertexStaging.destroy();
    indexStaging.destroy();
    
    m_models[blockID] = std::move(modelData);
    
    std::cout << "⚠ Created magenta fallback cube for BlockID " << (int)blockID << std::endl;
}

void VulkanModelRenderer::updateChunkInstances(VoxelChunk* chunk, uint32_t islandID, const glm::vec3& chunkOffset) {
    if (!chunk) return;
    
    // Clear existing batches for this chunk
    m_chunkBatches[chunk].clear();
    
    // Build new batches from chunk's model instances
    auto& registry = BlockTypeRegistry::getInstance();
    
    // Iterate through all possible OBJ block types
    for (uint8_t blockID = 0; blockID < 255; blockID++) {
        const BlockTypeInfo* info = registry.getBlockType(blockID);
        if (!info || info->renderType != BlockRenderType::OBJ) continue;
        
        const std::vector<glm::vec3>& positions = chunk->getModelInstances(blockID);
        if (positions.empty()) continue;
        
        std::cout << "[ModelRenderer] Found " << positions.size() << " instances of BlockID " << (int)blockID 
                  << " (" << info->name << ") in chunk at " << chunkOffset.x << "," << chunkOffset.y << "," << chunkOffset.z 
                  << " islandID=" << islandID << std::endl;
        
        // Load model if not already loaded
        if (m_models.find(blockID) == m_models.end()) {
            loadModel(blockID, info->assetPath);
        }
        
        // Create batch
        DrawBatch batch;
        batch.blockID = blockID;
        batch.needsUpload = true;
        
        for (const glm::vec3& localPos : positions) {
            InstanceData inst;
            inst.position = chunkOffset + localPos;
            inst.islandID = islandID;
            inst.blockID = blockID;
            inst.padding = 0;
            batch.instances.push_back(inst);
        }
        
        m_chunkBatches[chunk].push_back(std::move(batch));
    }
}

void VulkanModelRenderer::unregisterChunk(VoxelChunk* chunk) {
    m_chunkBatches.erase(chunk);
}

void VulkanModelRenderer::updateIslandTransform(uint32_t islandID, const glm::mat4& transform) {
    m_islandTransforms[islandID] = transform;
    
    // Upload to GPU
    if (m_islandTransformBuffer) {
        void* data = m_islandTransformBuffer->map();
        if (data && islandID < 64) {
            memcpy((char*)data + islandID * sizeof(glm::mat4), &transform, sizeof(glm::mat4));
        }
        m_islandTransformBuffer->unmap();
    }
}

void VulkanModelRenderer::rebuildInstanceBuffer() {
    m_instanceData.clear();
    
    // Collect all instances from all chunks
    for (const auto& [chunk, batches] : m_chunkBatches) {
        for (const auto& batch : batches) {
            m_instanceData.insert(m_instanceData.end(), batch.instances.begin(), batch.instances.end());
        }
    }
    
    // Upload to GPU
    if (!m_instanceData.empty() && m_instanceBuffer) {
        size_t dataSize = m_instanceData.size() * sizeof(InstanceData);
        if (dataSize > m_instanceBuffer->getSize()) {
            // Resize buffer
            m_instanceBuffer->destroy();
            m_instanceBuffer->create(m_allocator, dataSize * 2,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VMA_MEMORY_USAGE_CPU_TO_GPU);
        }
        
        void* data = m_instanceBuffer->map();
        memcpy(data, m_instanceData.data(), dataSize);
        m_instanceBuffer->unmap();
    }
}

void VulkanModelRenderer::renderToGBuffer(VkCommandBuffer cmd, const glm::mat4& viewProjection, const glm::mat4& view) {
    if (m_chunkBatches.empty()) {
        static bool once = true;
        if (once) {
            std::cout << "[ModelRenderer] No chunk batches to render" << std::endl;
            once = false;
        }
        return;
    }
    
    if (!m_pipeline) {
        std::cerr << "[ModelRenderer] No pipeline!" << std::endl;
        return;
    }
    
    rebuildInstanceBuffer();
    
    if (m_instanceData.empty()) {
        static bool once = true;
        if (once) {
            std::cout << "[ModelRenderer] No instance data after rebuild" << std::endl;
            once = false;
        }
        return;
    }
    
    // Bind instance buffer (shared by both pipelines)
    VkBuffer instanceBuffers[] = { m_instanceBuffer->getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 1, 1, instanceBuffers, offsets);
    
    // Get current time for animations
    static auto startTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float>(currentTime - startTime).count();
    
    // Bind pipeline once (single unified pipeline)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    
    if (m_descriptorSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                               0, 1, &m_descriptorSet, 0, nullptr);
    }
    
    // Push constants: mat4 viewProjection + float time (all models)
    struct alignas(16) PushConstants {
        glm::mat4 viewProjection;  // 64 bytes
        float time;                // 4 bytes
        float padding[3];          // 12 bytes padding
    } pushConstants = { viewProjection, time, {0,0,0} };
    
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                      0, sizeof(PushConstants), &pushConstants);
    
    // Draw each model type
    uint32_t instanceOffset = 0;
    for (const auto& [chunk, batches] : m_chunkBatches) {
        for (const auto& batch : batches) {
            auto it = m_models.find(batch.blockID);
            if (it == m_models.end()) continue;
            
            const ModelData& model = it->second;
            
            // Bind model vertex/index buffers
            VkBuffer vertexBuffers[] = { model.vertexBuffer->getBuffer() };
            VkDeviceSize vertexOffsets[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
            
            // Draw instanced
            vkCmdDrawIndexed(cmd, model.indexCount, batch.instances.size(), 0, 0, instanceOffset);
            
            instanceOffset += batch.instances.size();
        }
    }
}

void VulkanModelRenderer::createDescriptors() {
    VkDevice device = m_context->getDevice();
    
    // Descriptor set layout
    VkDescriptorSetLayoutBinding islandTransformBinding{};
    islandTransformBinding.binding = 0;
    islandTransformBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    islandTransformBinding.descriptorCount = 1;
    islandTransformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &islandTransformBinding;
    
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_descriptorSetLayout);
    
    // Descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool);
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    
    vkAllocateDescriptorSets(device, &allocInfo, &m_descriptorSet);
    
    // Update descriptor set
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_islandTransformBuffer->getBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = m_islandTransformBuffer->getSize();
    
    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;
    
    vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
}

void VulkanModelRenderer::createPipeline() {
    VkDevice device = m_context->getDevice();
    
    // Get exe directory for shader loading
    std::string exeDir;
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exePath(buffer);
    exeDir = exePath.parent_path().string();
#else
    exeDir = std::filesystem::current_path().string();
#endif
    
    // Load shaders
    auto loadShaderModule = [device](const char* filepath) -> VkShaderModule {
        std::ifstream file(filepath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open shader: " << filepath << std::endl;
            return VK_NULL_HANDLE;
        }
        
        size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = buffer.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
        
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            std::cerr << "Failed to create shader module: " << filepath << std::endl;
            return VK_NULL_HANDLE;
        }
        
        return shaderModule;
    };
    
    std::string vertPath = exeDir + "/shaders/vulkan/model_instance.vert.spv";
    std::string fragPath = exeDir + "/shaders/vulkan/model_instance.frag.spv";
    
    VkShaderModule vertShader = loadShaderModule(vertPath.c_str());
    VkShaderModule fragShader = loadShaderModule(fragPath.c_str());
    
    if (!vertShader || !fragShader) {
        std::cerr << "Failed to load model instance shaders" << std::endl;
        return;
    }
    
    VkPipelineShaderStageCreateInfo vertStageInfo{};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertShader;
    vertStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragStageInfo{};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragShader;
    fragStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };
    
    // Vertex input: stream 0 = model vertices, stream 1 = instance data
    VkVertexInputBindingDescription bindings[2];
    bindings[0].binding = 0;
    bindings[0].stride = 8 * sizeof(float);  // pos(3) + normal(3) + uv(2)
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(InstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    
    VkVertexInputAttributeDescription attributes[6];
    // Stream 0: model attributes
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;  // position
    attributes[0].offset = 0;
    
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;  // normal
    attributes[1].offset = 3 * sizeof(float);
    
    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;  // texcoord
    attributes[2].offset = 6 * sizeof(float);
    
    // Stream 1: instance attributes
    attributes[3].binding = 1;
    attributes[3].location = 3;
    attributes[3].format = VK_FORMAT_R32G32B32_SFLOAT;  // instance position
    attributes[3].offset = 0;
    
    attributes[4].binding = 1;
    attributes[4].location = 4;
    attributes[4].format = VK_FORMAT_R32_UINT;  // instance island ID
    attributes[4].offset = 3 * sizeof(float);
    
    attributes[5].binding = 1;
    attributes[5].location = 5;
    attributes[5].format = VK_FORMAT_R32_UINT;  // instance block ID
    attributes[5].offset = 4 * sizeof(float);
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 2;
    vertexInputInfo.pVertexBindingDescriptions = bindings;
    vertexInputInfo.vertexAttributeDescriptionCount = 6;
    vertexInputInfo.pVertexAttributeDescriptions = attributes;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_context->getSwapchainExtent().width;
    viewport.height = (float)m_context->getSwapchainExtent().height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = m_context->getSwapchainExtent();
    
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    
    // G-Buffer outputs (4 attachments)
    VkPipelineColorBlendAttachmentState colorBlendAttachments[4] = {};
    for (int i = 0; i < 4; i++) {
        colorBlendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachments[i].blendEnable = VK_FALSE;
    }
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 4;
    colorBlending.pAttachments = colorBlendAttachments;
    
    // Push constants: mat4 viewProjection + float time + padding (80 bytes total)
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 80;  // mat4(64) + float(4) + padding(12)
    
    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout" << std::endl;
        vkDestroyShaderModule(device, vertShader, nullptr);
        vkDestroyShaderModule(device, fragShader, nullptr);
        return;
    }
    
    // Create pipeline (using deferred G-buffer render pass)
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_gbufferRenderPass;
    pipelineInfo.subpass = 0;
    
    if (vkCreateGraphicsPipelines(device, m_context->pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline" << std::endl;
    }
    
    vkDestroyShaderModule(device, vertShader, nullptr);
    vkDestroyShaderModule(device, fragShader, nullptr);
    
    std::cout << "✓ VulkanModelRenderer pipeline created" << std::endl;
}


