#pragma once

/**
 * Global Engine Parameters
 * 
 * Centralized location for tweaking engine behavior, performance,
 * and visual settings without needing to hunt through code files.
 * 
 * Add new parameter categories here as needed.
 */

namespace EngineParameters {

// =============================================================================
// POST-PROCESSING EFFECTS
// =============================================================================

namespace PostProcessing {
    // === GODRAY SETTINGS ===
    
    // Performance settings
    static constexpr int GODRAY_SAMPLES = 32;           // Raymarching samples (16 = good performance, 32 = better quality, 64 = cinematic)
    
    // Intensity settings
    static constexpr float GODRAY_INTENSITY = 0.05f;    // Overall godray brightness (0.0 = off, 0.5 = strong, 1.0 = overwhelming)
    static constexpr float GODRAY_WEIGHT = 0.3f;       // Light weight per sample (lower = more subtle)
    static constexpr float GODRAY_DECAY = 0.9f;       // Light decay along ray (higher = longer rays)
    static constexpr float GODRAY_DENSITY = 0.5f;      // Sampling density (lower = more spread out)
    
    // Blending settings
    static constexpr float GODRAY_BLEND_FACTOR = 0.2f;  // How much godrays contribute to final image (0.0-1.0)
    
    // === TONE MAPPING SETTINGS ===
    static constexpr float HDR_EXPOSURE = 0.3f;         // HDR exposure adjustment
    static constexpr float GAMMA_CORRECTION = 2.2f;     // Gamma correction value
    
    // === DEBUG SETTINGS ===
    static constexpr bool ENABLE_POST_PROCESSING = true;
    static constexpr bool ENABLE_GODRAYS = true;        
    static constexpr bool ENABLE_TONE_MAPPING = true;   
}

// =============================================================================
// FUTURE PARAMETER CATEGORIES (examples)
// =============================================================================

/*
namespace Rendering {
    static constexpr int SHADOW_MAP_SIZE = 8192;
    static constexpr int MAX_CASCADES = 2;
    static constexpr float LOD_DISTANCE = 1000.0f;
}

namespace Physics {
    static constexpr float GRAVITY = -9.81f;
    static constexpr int MAX_PHYSICS_STEPS = 8;
    static constexpr float TIME_STEP = 1.0f / 60.0f;
}

namespace World {
    static constexpr int CHUNK_SIZE = 32;
    static constexpr int RENDER_DISTANCE = 10;
    static constexpr float ISLAND_SPACING = 500.0f;
}

namespace Performance {
    static constexpr int TARGET_FPS = 60;
    static constexpr bool ENABLE_VSYNC = true;
    static constexpr int MAX_PARTICLES = 10000;
}
*/

} // namespace EngineParameters