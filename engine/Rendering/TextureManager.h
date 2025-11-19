// TextureManager.h - Handles loading texture data for Vulkan
#pragma once

#include <cstdint>
#include <string>

struct TextureData {
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    
    ~TextureData();
    bool isValid() const { return pixels != nullptr; }
    void free();
};

class TextureManager
{
public:
    TextureManager();
    ~TextureManager();

    // Initialize with asset path detection
    bool initialize();
    
    // Load raw texture data (for Vulkan)
    TextureData loadTextureData(const std::string& filename);
    
    // Get asset base path
    const std::string& getAssetPath() const { return m_assetBasePath; }

private:
    std::string m_assetBasePath;
    
    // Helper functions
    std::string findAssetPath();
};

// Global texture manager instance
extern TextureManager* g_textureManager;
