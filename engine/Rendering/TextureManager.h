// TextureManager.h - Handles loading and managing textures
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Avoid leaking GL headers here; rely on glad in .cpp
using GLuint = unsigned int;

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
    
    // Load texture from file
    GLuint loadTexture(const std::string& filepath);
    
    // Load texture with specific settings
    GLuint loadTexture(const std::string& filepath, bool generateMipmaps, bool pixelArt = false);
    
    // Load raw texture data without creating OpenGL texture
    TextureData loadTextureData(const std::string& filename);
    
    // Get existing texture by name
    GLuint getTexture(const std::string& name);
    
    // Unload specific texture
    void unloadTexture(const std::string& name);
    
    // Unload all textures
    void unloadAllTextures();
    
    // Create texture from raw data
    GLuint createTexture(const unsigned char* data, int width, int height, int channels, bool pixelArt = false);
    
    // Get asset base path
    const std::string& getAssetPath() const { return m_assetBasePath; }

private:
    std::unordered_map<std::string, GLuint> m_textures;
    std::string m_assetBasePath;
    
    // Helper functions
    std::string getFileName(const std::string& filepath);
    std::string findAssetPath();
    void setTextureParameters(bool generateMipmaps, bool pixelArt);
};

// Global texture manager instance
extern TextureManager* g_textureManager;
