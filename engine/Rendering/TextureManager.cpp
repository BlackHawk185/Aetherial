// TextureManager.cpp - Implementation of texture loading for Vulkan
#include "TextureManager.h"
#include "stb_image.h"
#include <iostream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

// Global texture manager instance
TextureManager* g_textureManager = nullptr;

TextureData::~TextureData()
{
    free();
}

void TextureData::free()
{
    if (pixels) {
        stbi_image_free(pixels);
        pixels = nullptr;
    }
}

TextureManager::TextureManager()
{
}

TextureManager::~TextureManager()
{
}

bool TextureManager::initialize()
{
    // Skip if already initialized
    if (!m_assetBasePath.empty()) {
        std::cout << "   ├─ TextureManager already initialized with path: " << m_assetBasePath << std::endl;
        return true;
    }
    
    std::cout << "   ├─ Initializing TextureManager..." << std::endl;
    m_assetBasePath = findAssetPath();
    if (m_assetBasePath.empty()) {
        std::cerr << "❌ Failed to locate assets directory!" << std::endl;
        return false;
    }
    std::cout << "📁 Asset path: " << m_assetBasePath << std::endl;
    return true;
}

std::string TextureManager::findAssetPath()
{
    namespace fs = std::filesystem;
    
    // Get executable directory
    std::string exeDir;
    
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exePath(buffer);
    exeDir = exePath.parent_path().string();
#else
    exeDir = fs::current_path().string();
#endif
    
    std::cout << "   ├─ Searching for assets from exe dir: " << exeDir << std::endl;
    
    // Search order: bin/assets, assets, ../assets
    std::vector<std::string> searchPaths = {
        exeDir + "/assets/",
        exeDir + "/../assets/",
        fs::current_path().string() + "/assets/",
        fs::current_path().string() + "/../assets/"
    };
    
    for (const auto& path : searchPaths) {
        fs::path testPath(path + "textures");
        std::cout << "   ├─ Checking: " << testPath.string() << std::endl;
        if (fs::exists(testPath) && fs::is_directory(testPath)) {
            std::cout << "   └─ ✅ Found!" << std::endl;
            return path;
        }
    }
    
    std::cerr << "   └─ ❌ No valid asset path found!" << std::endl;
    return "";
}

TextureData TextureManager::loadTextureData(const std::string& filename)
{
    TextureData result;
    
    stbi_set_flip_vertically_on_load(false);
    
    // Try asset path first
    if (!m_assetBasePath.empty()) {
        std::string fullPath = m_assetBasePath + "textures/" + filename;
        result.pixels = stbi_load(fullPath.c_str(), &result.width, &result.height, &result.channels, 4); // Force RGBA
        result.channels = 4; // We forced RGBA
    }
    
    // Fallback to direct path
    if (!result.pixels) {
        result.pixels = stbi_load(filename.c_str(), &result.width, &result.height, &result.channels, 4);
        result.channels = 4;
    }
    
    return result;
}
