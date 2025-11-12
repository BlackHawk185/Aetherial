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
    // === TONE MAPPING SETTINGS ===
    static constexpr float HDR_EXPOSURE = 0.4f;         // HDR exposure adjustment
    static constexpr float GAMMA_CORRECTION = 2.2f;     // Gamma correction value
    
    // === DEBUG SETTINGS ===
    static constexpr bool ENABLE_POST_PROCESSING = true;
    static constexpr bool ENABLE_TONE_MAPPING = true;   
}

// =============================================================================
// VOLUMETRIC CLOUDS
// =============================================================================

namespace Clouds {
    // === PERFORMANCE SETTINGS ===
    static constexpr int RAYMARCH_SAMPLES = 32;         // Cloud raymarching steps (32 = fast, 64 = balanced, 128 = quality)
    static constexpr int NOISE_TEXTURE_SIZE = 128;      // 3D noise resolution (64 = 256KB, 128 = 2MB, 256 = 16MB)
    
    // === CLOUD VOLUME SETTINGS ===
    // 3D noise naturally creates varied height clouds - just define min/max bounds
    static constexpr float CLOUD_BASE_MIN_HEIGHT = -100.0f;    // Bottom of cloud volume (world units)
    static constexpr float CLOUD_BASE_MAX_HEIGHT = 300.0f;   // Top of cloud volume (world units)
    
    // === APPEARANCE SETTINGS ===
    static constexpr float CLOUD_COVERAGE = 0.5f;       // Cloud coverage (0.0 = clear, 1.0 = overcast)
    static constexpr float CLOUD_DENSITY = 0.5f;        // Cloud density multiplier (higher = thicker/darker)
    static constexpr float CLOUD_SPEED = 0.5f;          // Wind speed for cloud movement
    static constexpr float CLOUD_SCALE = 0.001f;        // World-space scale for noise sampling
    
    // === LIGHTING SETTINGS ===
    static constexpr float LIGHT_ABSORPTION = 2.0f;     // Beer-Lambert absorption coefficient
    static constexpr float AMBIENT_STRENGTH = 0.3f;     // Ambient light contribution
    
    // === DEBUG SETTINGS ===
    static constexpr bool ENABLE_CLOUDS = true;          // Master cloud toggle
    static constexpr bool ENABLE_CLOUD_SHADOWS = true;  // Cloud shadows on terrain/islands
}


} // namespace EngineParameters