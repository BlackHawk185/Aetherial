// ElementRecipes.cpp - Element combination recipes
#include "ElementRecipes.h"
#include "BlockType.h"
#include <sstream>
#include <algorithm>

std::string ElementQueue::toString() const {
    std::string result;
    for (auto elem : elements) {
        result += std::to_string(static_cast<int>(elem));
    }
    return result;
}

std::string ElementQueue::toFormula() const {
    if (elements.empty()) return "";
    
    // Count element occurrences
    std::unordered_map<Element, int> counts;
    for (auto elem : elements) {
        counts[elem]++;
    }
    
    // Build formula string (e.g., H2O)
    // Sort by element for consistency
    std::vector<std::pair<Element, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), 
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    std::stringstream ss;
    for (const auto& [elem, count] : sorted) {
        ss << ElementRecipeSystem::getElementSymbol(elem);
        if (count > 1) {
            ss << count;  // Subscript numbers would be nice but ASCII for now
        }
    }
    return ss.str();
}

BlockRecipe BlockRecipe::create(std::initializer_list<Element> elemList, uint8_t block, 
                                const std::string& name, const std::string& formula) {
    BlockRecipe recipe;
    recipe.blockID = block;
    recipe.name = name;
    recipe.formula = formula;
    
    // Count elements (order doesn't matter)
    for (auto elem : elemList) {
        recipe.elements[elem]++;
    }
    
    return recipe;
}

ElementRecipeSystem::ElementRecipeSystem() {
    initializeRecipes();
}

void ElementRecipeSystem::initializeRecipes() {
    m_recipes.clear();
    m_recipeMap.clear();
    
    // Simple recipes (single elements or pure compounds)
    // Order doesn't matter - we count element occurrences
    
    // Pure elements
    m_recipes.push_back(BlockRecipe::create({Element::C}, BlockID::COAL, "Coal", "C"));
    m_recipes.push_back(BlockRecipe::create({Element::Fe}, BlockID::IRON_BLOCK, "Iron Block", "Fe"));
    m_recipes.push_back(BlockRecipe::create({Element::Au}, BlockID::GOLD_BLOCK, "Gold Block", "Au"));
    m_recipes.push_back(BlockRecipe::create({Element::Cu}, BlockID::COPPER_BLOCK, "Copper Block", "Cu"));
    
    // Water - H₂O (2 hydrogen, 1 oxygen)
    m_recipes.push_back(BlockRecipe::create({Element::H, Element::H, Element::O}, BlockID::WATER, "Water", "H2O"));
    
    // Stone - SiO₂ (1 silicon, 2 oxygen)
    m_recipes.push_back(BlockRecipe::create({Element::Si, Element::O, Element::O}, BlockID::STONE, "Stone", "SiO2"));
    
    // Sand - SiO (simplified, 1 silicon, 1 oxygen)
    m_recipes.push_back(BlockRecipe::create({Element::Si, Element::O}, BlockID::SAND, "Sand", "SiO"));
    
    // Salt - NaCl (1 sodium, 1 chlorine)
    m_recipes.push_back(BlockRecipe::create({Element::Na, Element::Cl}, BlockID::SALT_BLOCK, "Salt", "NaCl"));
    
    // Limestone - CaCO₃ (1 calcium, 1 carbon, 3 oxygen)
    m_recipes.push_back(BlockRecipe::create({Element::Ca, Element::C, Element::O, Element::O, Element::O}, 
                                           BlockID::LIMESTONE, "Limestone", "CaCO3"));
    
    // Ice - H₂O + Cold (different recipe to avoid collision with water)
    // Using Helium as a "cold" element since it's the coldest element
    m_recipes.push_back(BlockRecipe::create({Element::H, Element::H, Element::O, Element::He}, BlockID::ICE, "Ice", "H2O·He"));
    
    // Diamond - C₄ (4 carbon atoms in crystalline structure)
    m_recipes.push_back(BlockRecipe::create({Element::C, Element::C, Element::C, Element::C}, 
                                           BlockID::DIAMOND_BLOCK, "Diamond", "C4"));
    
    // Quantum Field Generator - AuFeC₄ (Gold + Iron + Diamond structure)
    // Requires advanced materials: gold conductor, iron framework, carbon matrix
    m_recipes.push_back(BlockRecipe::create({Element::Au, Element::Fe, Element::C, Element::C, Element::C, Element::C}, 
                                           BlockID::QUANTUM_FIELD_GENERATOR, "Quantum Field Generator", "AuFeC4"));
    
    // Build lookup map (sorted element count signature -> recipe)
    for (const auto& recipe : m_recipes) {
        std::string key = createRecipeKey(recipe.elements);
        m_recipeMap[key] = &recipe;
    }
}

std::string ElementRecipeSystem::createRecipeKey(const std::unordered_map<Element, int>& elements) const {
    // Create sorted key: "element_id:count,element_id:count,..."
    std::vector<std::pair<Element, int>> sorted(elements.begin(), elements.end());
    std::sort(sorted.begin(), sorted.end());
    
    std::stringstream ss;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) ss << ",";
        ss << static_cast<int>(sorted[i].first) << ":" << sorted[i].second;
    }
    return ss.str();
}

const BlockRecipe* ElementRecipeSystem::matchRecipe(const ElementQueue& queue) const {
    if (queue.isEmpty()) return nullptr;
    
    // Count elements in queue
    std::unordered_map<Element, int> counts;
    for (auto elem : queue.elements) {
        counts[elem]++;
    }
    
    // Create key and lookup
    std::string key = createRecipeKey(counts);
    auto it = m_recipeMap.find(key);
    if (it != m_recipeMap.end()) {
        return it->second;
    }
    
    return nullptr;
}

const char* ElementRecipeSystem::getElementName(Element elem) {
    switch (elem) {
        case Element::H:  return "Hydrogen";
        case Element::He: return "Helium";
        case Element::Li: return "Lithium";
        case Element::C:  return "Carbon";
        case Element::N:  return "Nitrogen";
        case Element::O:  return "Oxygen";
        case Element::F:  return "Fluorine";
        case Element::Ne: return "Neon";
        case Element::Na: return "Sodium";
        case Element::Mg: return "Magnesium";
        case Element::Al: return "Aluminum";
        case Element::Si: return "Silicon";
        case Element::P:  return "Phosphorus";
        case Element::S:  return "Sulfur";
        case Element::Cl: return "Chlorine";
        case Element::K:  return "Potassium";
        case Element::Ca: return "Calcium";
        case Element::Fe: return "Iron";
        case Element::Cu: return "Copper";
        case Element::Au: return "Gold";
        default: return "Unknown";
    }
}

const char* ElementRecipeSystem::getElementSymbol(Element elem) {
    switch (elem) {
        case Element::H:  return "H";
        case Element::He: return "He";
        case Element::Li: return "Li";
        case Element::C:  return "C";
        case Element::N:  return "N";
        case Element::O:  return "O";
        case Element::F:  return "F";
        case Element::Ne: return "Ne";
        case Element::Na: return "Na";
        case Element::Mg: return "Mg";
        case Element::Al: return "Al";
        case Element::Si: return "Si";
        case Element::P:  return "P";
        case Element::S:  return "S";
        case Element::Cl: return "Cl";
        case Element::K:  return "K";
        case Element::Ca: return "Ca";
        case Element::Fe: return "Fe";
        case Element::Cu: return "Cu";
        case Element::Au: return "Au";
        default: return "?";
    }
}

uint32_t ElementRecipeSystem::getElementColor(Element elem) {
    // Returns ImU32 color (ABGR format: 0xAABBGGRR)
    // Color scheme based on periodic table groups
    
    switch (elem) {
        // Group 1: Alkali metals (purple)
        case Element::H:  // Hydrogen (special case, but in group 1)
        case Element::Li:
        case Element::Na:
        case Element::K:
            return 0xDCB48CC8;  // Purple: RGBA(200, 140, 180, 220)
        
        // Group 2: Alkaline earth metals (purple)
        case Element::Mg:
        case Element::Ca:
            return 0xDCB48CC8;  // Purple
        
        // Groups 13-16: Non-metals and metalloids (green)
        case Element::C:
        case Element::N:
        case Element::O:
        case Element::Si:
        case Element::P:
        case Element::S:
        case Element::Al:
            return 0xDC8CC88C;  // Green: RGBA(140, 200, 140, 220)
        
        // Group 17: Halogens (yellow)
        case Element::F:
        case Element::Cl:
            return 0xDC8CDCF0;  // Yellow: RGBA(240, 220, 140, 220)
        
        // Group 18: Noble gases (light blue)
        case Element::He:
        case Element::Ne:
            return 0xDCF0DCB4;  // Light blue: RGBA(180, 220, 240, 220)
        
        // Transition metals (orange)
        case Element::Fe:
        case Element::Cu:
        case Element::Au:
            return 0xDC8CB4F0;  // Orange: RGBA(240, 180, 140, 220)
        
        // None/Unknown (gray)
        case Element::None:
        default:
            return 0xC8969696;  // Gray: RGBA(150, 150, 150, 200)
    }
}
