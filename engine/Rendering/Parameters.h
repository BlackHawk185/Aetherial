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
    static constexpr float GODRAY_INTENSITY = 0.1f;    // Overall godray brightness (0.0 = off, 0.5 = strong, 1.0 = overwhelming)
    static constexpr float GODRAY_WEIGHT = 0.3f;       // Light weight per sample (lower = more subtle)
    static constexpr float GODRAY_DECAY = 0.95f;       // Light decay along ray (higher = longer rays)
    static constexpr float GODRAY_DENSITY = 0.01f;      // Sampling density (lower = more spread out)
    
    // Blending settings
    static constexpr float GODRAY_BLEND_FACTOR = 0.2f;  // How much godrays contribute to final image (0.0-1.0)
    
    // === TONE MAPPING SETTINGS ===
    static constexpr float HDR_EXPOSURE = 0.4f;         // HDR exposure adjustment
    static constexpr float GAMMA_CORRECTION = 2.2f;     // Gamma correction value
    
    // === DEBUG SETTINGS ===
    static constexpr bool ENABLE_POST_PROCESSING = true;
    static constexpr bool ENABLE_GODRAYS = false;        // Disabled
    static constexpr bool ENABLE_TONE_MAPPING = true;   
}

// =============================================================================
// VOLUMETRIC CLOUDS
// =============================================================================

namespace Clouds {
    // === PERFORMANCE SETTINGS ===
    static constexpr int RAYMARCH_SAMPLES = 64;         // Cloud raymarching steps (32 = fast, 64 = balanced, 128 = quality)
    static constexpr int NOISE_TEXTURE_SIZE = 128;      // 3D noise resolution (64 = 256KB, 128 = 2MB, 256 = 16MB)
    
    // === CLOUD VOLUME SETTINGS ===
    // 3D noise naturally creates varied height clouds - just define min/max bounds
    static constexpr float CLOUD_BASE_MIN_HEIGHT = -100.0f;    // Bottom of cloud volume (world units)
    static constexpr float CLOUD_BASE_MAX_HEIGHT = 300.0f;   // Top of cloud volume (world units)
    
    // === APPEARANCE SETTINGS ===
    static constexpr float CLOUD_COVERAGE = 0.2f;       // Cloud coverage (0.0 = clear, 1.0 = overcast)
    static constexpr float CLOUD_DENSITY = 0.8f;        // Cloud density multiplier (higher = thicker/darker)
    static constexpr float CLOUD_SPEED = 0.1f;          // Wind speed for cloud movement
    static constexpr float CLOUD_SCALE = 0.001f;        // World-space scale for noise sampling
    
    // === LIGHTING SETTINGS ===
    static constexpr float LIGHT_ABSORPTION = 2.0f;     // Beer-Lambert absorption coefficient
    static constexpr float AMBIENT_STRENGTH = 0.3f;     // Ambient light contribution
    
    // === DEBUG SETTINGS ===
    static constexpr bool ENABLE_CLOUDS = true;         // Master cloud toggle
    static constexpr bool ENABLE_CLOUD_SHADOWS = true;  // Cloud shadows on terrain/islands
}


} // namespace EngineParameters