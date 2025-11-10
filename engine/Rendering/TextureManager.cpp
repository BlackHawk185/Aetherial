// TextureManager.cpp - Implementation of texture loading and management
#include "TextureManager.h"
#include <glad/gl.h>
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
    unloadAllTextures();
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


GLuint TextureManager::loadTexture(const std::string& filepath)
{
    return loadTexture(filepath, true, false);
}

GLuint TextureManager::loadTexture(const std::string& filepath, bool generateMipmaps, bool pixelArt)
{
    // Check if texture is already loaded
    std::string filename = getFileName(filepath);
    auto it = m_textures.find(filename);
    if (it != m_textures.end()) {
        return it->second;
    }
    
    // Try loading from provided path first
    stbi_set_flip_vertically_on_load(false);
    int width, height, channels;
    unsigned char* data = stbi_load(filepath.c_str(), &width, &height, &channels, 0);
    
    // If failed and we have an asset path, try relative to assets
    if (!data && !m_assetBasePath.empty()) {
        std::string assetPath = m_assetBasePath + "textures/" + filename;
        data = stbi_load(assetPath.c_str(), &width, &height, &channels, 0);
    }
    
    if (!data) {
        return 0;
    }
    
    // Create OpenGL texture
    GLuint textureID = createTexture(data, width, height, channels, pixelArt);
    
    // Free image data
    stbi_image_free(data);
    
    if (textureID != 0) {
        m_textures[filename] = textureID;
    }
    
    return textureID;
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

GLuint TextureManager::getTexture(const std::string& name)
{
    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        return it->second;
    }
    return 0;
}

void TextureManager::unloadTexture(const std::string& name)
{
    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        glDeleteTextures(1, &it->second);
        m_textures.erase(it);
        std::cout << "Unloaded texture: " << name << std::endl;
    }
}

void TextureManager::unloadAllTextures()
{
    for (auto& pair : m_textures) {
        glDeleteTextures(1, &pair.second);
    }
    m_textures.clear();
    std::cout << "Unloaded all textures" << std::endl;
}

GLuint TextureManager::createTexture(const unsigned char* data, int width, int height, int channels, bool pixelArt)
{
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    
    // Determine internal and external formats for core profile
    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    if (channels == 1) { internalFormat = GL_R8;   format = GL_RED; }
    else if (channels == 3) { internalFormat = GL_RGB8; format = GL_RGB; }
    else if (channels == 4) { internalFormat = GL_RGBA8; format = GL_RGBA; }
    else {
        std::cerr << "Unsupported number of channels: " << channels << std::endl;
        glDeleteTextures(1, &textureID);
        return 0;
    }

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    
    // Generate mipmaps for distant LOD
    glGenerateMipmap(GL_TEXTURE_2D);
    
    // Set texture parameters
    if (pixelArt) {
        // Crisp pixel art with mipmap blending
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        // Smooth filtering with mipmaps
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    
    // Wrapping
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    // Mipmaps disabled in this build profile to avoid missing GL loader symbols
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    return textureID;
}

std::string TextureManager::getFileName(const std::string& filepath)
{
    std::filesystem::path path(filepath);
    return path.filename().string();
}
