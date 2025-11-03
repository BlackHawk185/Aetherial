// HUD.cpp - Heads-Up Display implementation
#include "HUD.h"
#include "../World/BlockType.h"
#include "../World/ElementRecipes.h"
#include <imgui.h>
#include <sstream>
#include <iomanip>

HUD::HUD() {
    // Initialize HUD state
}

HUD::~HUD() {
    // Cleanup if needed
}

void HUD::render(float deltaTime) {
    m_timeSinceLastUpdate += deltaTime;
    
    renderHealthBar();
    renderCurrentBlock();
    renderFPS();
    
    if (m_showDebugInfo) {
        renderDebugInfo();
    }
    
    if (!m_targetBlock.empty()) {
        renderTargetBlock();
    }
}

void HUD::renderHealthBar() {
    // OPTIMIZED: Use direct ImDrawList rendering instead of ImGui windows
    // This eliminates per-frame GPU sync overhead from ImGui::Begin/End
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    float healthPercent = m_health / m_maxHealth;
    
    // Health bar position (top-left)
    const float x = 10.0f;
    const float y = 10.0f;
    const float width = 200.0f;
    const float height = 20.0f;
    
    // Background
    drawList->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + width, y + height),
        IM_COL32(20, 20, 20, 200),
        3.0f
    );
    
    // Health fill
    drawList->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + width * healthPercent, y + height),
        IM_COL32(204, 25, 25, 255),  // Red health bar
        3.0f
    );
    
    // Border
    drawList->AddRect(
        ImVec2(x, y),
        ImVec2(x + width, y + height),
        IM_COL32(255, 255, 255, 255),
        3.0f,
        0,
        2.0f
    );
    
    // "Health" label above bar
    drawList->AddText(ImVec2(x, y - 18.0f), IM_COL32(255, 255, 255, 255), "Health");
}

void HUD::renderDebugInfo() {
    // OPTIMIZED: Use direct ImDrawList rendering instead of ImGui windows
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    const float x = 10.0f;
    float y = 80.0f;
    const float lineHeight = 20.0f;
    
    char buffer[256];
    
    snprintf(buffer, sizeof(buffer), "Position: %.1f, %.1f, %.1f", m_playerX, m_playerY, m_playerZ);
    drawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), buffer);
    y += lineHeight;
    
    snprintf(buffer, sizeof(buffer), "FPS: %.1f", m_fps);
    drawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), buffer);
    y += lineHeight;
    
    drawList->AddText(ImVec2(x, y), IM_COL32(200, 200, 200, 255), "Press F3 to toggle debug info");
}

void HUD::renderCurrentBlock() {
    // OPTIMIZED: Use direct ImDrawList rendering instead of ImGui windows
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Bottom center
    ImVec2 textSize = ImGui::CalcTextSize(m_currentBlock.c_str());
    float x = io.DisplaySize.x * 0.5f - textSize.x * 0.5f;
    float y = io.DisplaySize.y - 80.0f;
    
    drawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), m_currentBlock.c_str());
}

void HUD::renderTargetBlock() {
    // OPTIMIZED: Use direct ImDrawList rendering instead of ImGui windows
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Center, below crosshair
    ImVec2 blockNameSize = ImGui::CalcTextSize(m_targetBlock.c_str());
    float x = io.DisplaySize.x * 0.5f - blockNameSize.x * 0.5f;
    float y = io.DisplaySize.y * 0.5f + 30.0f;
    
    // Block name (light blue)
    drawList->AddText(ImVec2(x, y), IM_COL32(178, 178, 255, 255), m_targetBlock.c_str());
    
    // Chemical formula (if available) - green, below name
    if (!m_targetFormula.empty()) {
        ImVec2 formulaSize = ImGui::CalcTextSize(m_targetFormula.c_str());
        float formulaX = io.DisplaySize.x * 0.5f - formulaSize.x * 0.5f;
        drawList->AddText(ImVec2(formulaX, y + 20.0f), IM_COL32(128, 255, 128, 255), m_targetFormula.c_str());
    }
}

void HUD::renderFPS() {
    if (m_showDebugInfo) return; // Already shown in debug info
    
    // OPTIMIZED: Use direct ImDrawList rendering instead of ImGui windows
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    char fpsBuffer[32];
    snprintf(fpsBuffer, sizeof(fpsBuffer), "FPS: %.0f", m_fps);
    
    ImVec2 textSize = ImGui::CalcTextSize(fpsBuffer);
    float x = io.DisplaySize.x - textSize.x - 10.0f;  // Right-aligned with 10px margin
    float y = 10.0f;
    
    drawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), fpsBuffer);
}

void HUD::setPlayerPosition(float x, float y, float z) {
    m_playerX = x;
    m_playerY = y;
    m_playerZ = z;
}

void HUD::setPlayerHealth(float health, float maxHealth) {
    m_health = health;
    m_maxHealth = maxHealth;
}

void HUD::setCurrentBlock(const std::string& blockName) {
    m_currentBlock = blockName;
}

void HUD::setFPS(float fps) {
    m_fps = fps;
}

void HUD::setTargetBlock(const std::string& blockName, const std::string& formula) {
    m_targetBlock = blockName;
    m_targetFormula = formula;
}

void HUD::clearTargetBlock() {
    m_targetBlock = "";
    m_targetFormula = "";
}

void HUD::renderElementQueue(const ElementQueue& queue, const BlockRecipe* lockedRecipe,
                             const std::array<Element, 9>& hotbarElements) {
    ImGuiIO& io = ImGui::GetIO();
    
    // Hotbar dimensions (9 slots for elements 1-9)
    const float slotSize = 60.0f;
    const float slotPadding = 4.0f;
    const int numSlots = 9;  // Keys 1-9
    const float totalWidth = (slotSize + slotPadding) * numSlots - slotPadding;
    const float startX = (io.DisplaySize.x - totalWidth) * 0.5f;
    const float startY = io.DisplaySize.y - 100.0f;  // 100px from bottom
    
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Draw hotbar slots (using customizable hotbar)
    for (int i = 0; i < numSlots; ++i) {
        float x = startX + i * (slotSize + slotPadding);
        float y = startY;
        
        Element elem = hotbarElements[i];
        
        // Get element color (shared with periodic table)
        ImU32 elementColor = ElementRecipeSystem::getElementColor(elem);
        
        // Slot background with element color
        drawList->AddRectFilled(
            ImVec2(x, y),
            ImVec2(x + slotSize, y + slotSize),
            elementColor,
            4.0f  // Corner rounding
        );
        
        // Inner darker background for contrast (slightly transparent)
        drawList->AddRectFilled(
            ImVec2(x + 3, y + 3),
            ImVec2(x + slotSize - 3, y + slotSize - 3),
            IM_COL32(30, 30, 30, 180),
            3.0f
        );
        
        // Slot border
        drawList->AddRect(
            ImVec2(x, y),
            ImVec2(x + slotSize, y + slotSize),
            IM_COL32(200, 200, 200, 220),
            4.0f,
            0,
            2.5f  // Slightly thicker border
        );
        
        // Slot number (1-9) in top-left corner
        char numberStr[2];
        numberStr[0] = '1' + i;
        numberStr[1] = '\0';
        drawList->AddText(
            ImVec2(x + 4.0f, y + 2.0f),
            IM_COL32(220, 220, 220, 255),
            numberStr
        );
        
        // Element symbol (large, centered)
        std::string symbol = ElementRecipeSystem::getElementSymbol(elem);
        
        // Draw symbol larger using font size parameter
        ImFont* font = ImGui::GetFont();
        float fontSize = 28.0f;  // Larger font size for symbols
        ImVec2 symbolSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, symbol.c_str());
        
        drawList->AddText(
            font,
            fontSize,
            ImVec2(x + (slotSize - symbolSize.x) * 0.5f, y + (slotSize - symbolSize.y) * 0.5f - 2.0f),
            IM_COL32(220, 220, 220, 255),
            symbol.c_str()
        );
        
        // Element name (small, bottom)
        std::string name = ElementRecipeSystem::getElementName(elem);
        ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        drawList->AddText(
            ImVec2(x + (slotSize - nameSize.x) * 0.5f, y + slotSize - 14.0f),
            IM_COL32(150, 150, 150, 255),
            name.c_str()
        );
    }
    
    // Show current queue above hotbar (if not empty)
    if (!queue.isEmpty() || lockedRecipe) {
        const float queueY = startY - 45.0f;
        
        if (lockedRecipe) {
            // Show locked recipe
            std::string recipeText = "Locked: " + lockedRecipe->name + " (" + lockedRecipe->formula + ")";
            ImVec2 textSize = ImGui::CalcTextSize(recipeText.c_str());
            
            drawList->AddRectFilled(
                ImVec2(startX + (totalWidth - textSize.x) * 0.5f - 10.0f, queueY - 5.0f),
                ImVec2(startX + (totalWidth + textSize.x) * 0.5f + 10.0f, queueY + textSize.y + 5.0f),
                IM_COL32(20, 60, 20, 220),
                4.0f
            );
            
            drawList->AddText(
                ImVec2(startX + (totalWidth - textSize.x) * 0.5f, queueY),
                IM_COL32(100, 255, 100, 255),  // Green
                recipeText.c_str()
            );
        } else {
            // Show current queue formula
            std::string formula = queue.toFormula();
            ImVec2 formulaSize = ImGui::CalcTextSize(formula.c_str());
            
            drawList->AddRectFilled(
                ImVec2(startX + (totalWidth - formulaSize.x) * 0.5f - 10.0f, queueY - 5.0f),
                ImVec2(startX + (totalWidth + formulaSize.x) * 0.5f + 10.0f, queueY + formulaSize.y + 5.0f),
                IM_COL32(40, 40, 20, 220),
                4.0f
            );
            
            drawList->AddText(
                ImVec2(startX + (totalWidth - formulaSize.x) * 0.5f, queueY),
                IM_COL32(255, 255, 100, 255),  // Yellow
                formula.c_str()
            );
        }
    }
}
