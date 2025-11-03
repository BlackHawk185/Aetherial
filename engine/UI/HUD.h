// HUD.h - Heads-Up Display overlay system
#pragma once

#include <string>
#include <array>
#include "../World/ElementRecipes.h"  // Need full definition for Element

// Forward declarations
struct ElementQueue;
struct BlockRecipe;

// Simple HUD overlay using Dear ImGui
class HUD {
public:
    HUD();
    ~HUD();
    
    // Render the HUD (call every frame)
    void render(float deltaTime);
    
    // Element-based crafting UI with customizable hotbar
    void renderElementQueue(const ElementQueue& queue, const BlockRecipe* lockedRecipe,
                           const std::array<Element, 9>& hotbarElements);
    
    // Update HUD state
    void setPlayerPosition(float x, float y, float z);
    void setPlayerHealth(float health, float maxHealth);
    void setCurrentBlock(const std::string& blockName);
    void setFPS(float fps);
    void setTargetBlock(const std::string& blockName, const std::string& formula = ""); // Block player is looking at + formula
    void clearTargetBlock();
    
    // Toggle HUD elements
    void toggleDebugInfo() { m_showDebugInfo = !m_showDebugInfo; }
    void setShowDebugInfo(bool show) { m_showDebugInfo = show; }
    
private:
    // HUD element rendering methods
    void renderHealthBar();
    void renderDebugInfo();
    void renderCurrentBlock();
    void renderTargetBlock();
    void renderFPS();
    
    // State
    float m_playerX = 0.0f, m_playerY = 0.0f, m_playerZ = 0.0f;
    float m_health = 100.0f;
    float m_maxHealth = 100.0f;
    float m_fps = 60.0f;
    std::string m_currentBlock = "Stone";
    std::string m_targetBlock = "";
    std::string m_targetFormula = "";  // NEW: Chemical formula of target block
    bool m_showDebugInfo = false;
    
    // Timing
    float m_timeSinceLastUpdate = 0.0f;
};
