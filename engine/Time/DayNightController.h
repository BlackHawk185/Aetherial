// DayNightController.h - Simple day/night cycle for atmospheric lighting
#pragma once

#include "../Math/Vec3.h"

/**
 * Manages time-of-day progression and calculates sun/moon positions.
 * Updates GlobalLightingManager with current sun direction for shadow casting.
 * Designed to preserve multi-light support - this only controls the primary directional light.
 */
class DayNightController {
public:
    DayNightController();
    ~DayNightController() = default;

    // Core update - call once per frame
    void update(float deltaTime);
    
    // Time control
    void setTimeOfDay(float hours);        // 0.0-24.0 (0=midnight, 6=sunrise, 12=noon, 18=sunset)
    float getTimeOfDay() const { return m_currentTime; }
    
    void setTimeSpeed(float multiplier);   // How fast time passes (default: 60x = 24min real-time day)
    float getTimeSpeed() const { return m_timeSpeed; }
    
    void pause() { m_paused = true; }
    void resume() { m_paused = false; }
    bool isPaused() const { return m_paused; }
    
    // Lighting information
    Vec3 getSunDirection() const;          // Current sun direction vector (for shadow casting)
    Vec3 getMoonDirection() const;         // Moon is opposite the sun
    float getSunIntensity() const;         // 0.0 (night) to 1.0 (midday)
    
    // Sky colors for rendering
    struct SkyColors {
        Vec3 zenith;       // Color at top of sky
        Vec3 horizon;      // Color at horizon
        Vec3 sunColor;     // Sun disc color
        Vec3 moonColor;    // Moon disc color
        Vec3 fogColor;     // Atmospheric fog color
    };
    
    SkyColors getSkyColors() const;
    
    // Time periods for gameplay/events
    enum class Period {
        NIGHT,      // 0:00 - 5:00
        DAWN,       // 5:00 - 7:00
        MORNING,    // 7:00 - 11:00
        MIDDAY,     // 11:00 - 13:00
        AFTERNOON,  // 13:00 - 17:00
        DUSK,       // 17:00 - 19:00
        EVENING     // 19:00 - 24:00
    };
    
    Period getCurrentPeriod() const;
    const char* getPeriodName() const;

private:
    // Core state
    float m_currentTime;      // 0.0-24.0 hours
    float m_timeSpeed;        // Time multiplier (default 60x = 24min day)
    bool m_paused;
    
    // Internal calculations
    float calculateSunAngle() const;      // Angle in radians for sun position
    float smoothTransition(float t) const; // Smoothstep for transitions
};

// Global instance
extern DayNightController* g_dayNightController;
