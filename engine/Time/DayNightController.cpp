// DayNightController.cpp - Implementation of day/night cycle
#include "DayNightController.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Global instance
DayNightController* g_dayNightController = nullptr;

DayNightController::DayNightController()
    : m_currentTime(12.0f)     // Start at noon for nice lighting
    , m_timeSpeed(600.0f)       // 600x speed = 1 minute day cycle
    , m_paused(false)
    , m_moonPhase(14.765f)      // Start moon ~180 degrees from sun (half lunar cycle)
    , m_moonOrbitSpeed(1.0f / 29.53f)  // Moon takes 29.53 days to orbit (relative to sun's 1-day cycle)
{
}

void DayNightController::update(float deltaTime) {
    if (m_paused) return;
    
    // Convert deltaTime (seconds) to hours and apply time speed
    float timeIncrement = (deltaTime / 3600.0f) * m_timeSpeed;
    m_currentTime += timeIncrement;
    
    // Wrap around 24 hours
    while (m_currentTime >= 24.0f) {
        m_currentTime -= 24.0f;
    }
    while (m_currentTime < 0.0f) {
        m_currentTime += 24.0f;
    }
    
    // Update moon phase (independent from sun, much slower orbit)
    // Moon advances at 1/29.53 the rate of the sun
    float moonIncrement = (timeIncrement / 24.0f) * m_moonOrbitSpeed * 29.53f;
    m_moonPhase += moonIncrement;
    
    // Wrap moon phase around 29.53 day cycle
    while (m_moonPhase >= 29.53f) {
        m_moonPhase -= 29.53f;
    }
    while (m_moonPhase < 0.0f) {
        m_moonPhase += 29.53f;
    }
}

void DayNightController::setTimeOfDay(float hours) {
    m_currentTime = std::fmod(hours, 24.0f);
    if (m_currentTime < 0.0f) m_currentTime += 24.0f;
}

void DayNightController::setTimeSpeed(float multiplier) {
    m_timeSpeed = std::max(0.0f, multiplier);
}

float DayNightController::calculateSunAngle() const {
    // Sun angle: 0 degrees at sunrise (6:00), 90 degrees at noon, 180 at sunset (18:00)
    // Map 0-24 hours to 0-360 degrees (full rotation)
    float hourAngle = (m_currentTime / 24.0f) * 360.0f;
    
    // Offset so noon (12:00) is at zenith
    float sunAngle = hourAngle - 90.0f;
    
    return sunAngle * (M_PI / 180.0f); // Convert to radians
}

float DayNightController::calculateMoonAngle() const {
    // Moon has its own independent cycle (29.53 days)
    // Map 0-29.53 days to 0-360 degrees
    float moonAngle = (m_moonPhase / 29.53f) * 360.0f;
    
    // Offset so moon starts opposite the sun at initialization
    moonAngle -= 90.0f;
    
    return moonAngle * (M_PI / 180.0f); // Convert to radians
}

Vec3 DayNightController::getSunDirection() const {
    float angle = calculateSunAngle();
    
    // Sun moves in an arc across the sky
    // The sun should trace a single arc from east to west
    // Y component is the elevation (up/down)
    // X component is the horizontal movement (east/west)
    float elevation = std::sin(angle);
    float horizontalDistance = std::cos(angle);
    
    // Create directional vector (pointing FROM sun TO world, for lighting calculations)
    // The sun rises in the east (positive X), moves overhead, sets in the west (negative X)
    Vec3 sunDir(
        horizontalDistance,  // East-West movement along the arc
        -elevation,          // Up-Down (negative because light points down)
        0.0f                 // No north-south offset - keep sun on consistent path
    );
    
    return sunDir.normalized();
}

Vec3 DayNightController::getMoonDirection() const {
    float angle = calculateMoonAngle();
    
    // Moon moves in a different arc than the sun (independent orbital plane)
    // To create a tilted orbital plane, we'll use a different approach than the sun
    float elevation = std::sin(angle);
    float horizontalDistance = std::cos(angle);
    
    // Create directional vector (pointing FROM moon TO world, for lighting calculations)
    // Moon follows a similar but independent path on a slightly tilted plane
    // Instead of Z offset, we'll tilt the entire arc by rotating around X axis slightly
    Vec3 moonDir(
        horizontalDistance,      // East-West movement along the arc
        -elevation,              // Up-Down (negative because light points down)
        0.0f                     // No Z offset to avoid dual moon issue
    );
    
    return moonDir.normalized();
}

float DayNightController::getSunIntensity() const {
    float angle = calculateSunAngle();
    float elevation = std::sin(angle);
    
    // Sun is always active, brightness varies with elevation
    // Full brightness at zenith, dimmer near horizon, but never off
    // Map -1 to 1 elevation to 0.3 to 1.0 intensity
    return 0.3f + (elevation * 0.5f + 0.5f) * 0.7f;
}

float DayNightController::getMoonIntensity() const {
    float angle = calculateMoonAngle();
    float elevation = std::sin(angle);
    
    // Moon is always active, but much dimmer than sun
    // Map -1 to 1 elevation to 0.05 to 0.2 intensity
    return 0.05f + (elevation * 0.5f + 0.5f) * 0.15f;
}

float DayNightController::smoothTransition(float t) const {
    // Smoothstep function for smooth transitions
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

DayNightController::SkyColors DayNightController::getSkyColors() const {
    SkyColors colors;
    
    // Just 4 keyframes: Midnight, Dawn, Noon, Dusk
    struct ColorKeyframe {
        float time;
        Vec3 zenith;
        Vec3 horizon;
        Vec3 sunColor;
        Vec3 moonColor;
    };
    
    ColorKeyframe keyframes[] = {
        // Midnight (0:00)
        { 0.0f,  Vec3(0.01f, 0.01f, 0.05f), Vec3(0.05f, 0.05f, 0.15f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.8f, 0.8f, 0.9f) },
        
        // Dawn (6:00)
        { 6.0f,  Vec3(0.4f, 0.3f, 0.6f),    Vec3(1.0f, 0.5f, 0.3f),    Vec3(1.0f, 0.7f, 0.4f), Vec3(0.2f, 0.2f, 0.3f) },
        
        // Noon (12:00)
        { 12.0f, Vec3(0.3f, 0.5f, 0.9f),    Vec3(0.6f, 0.7f, 0.9f),    Vec3(1.0f, 1.0f, 0.95f), Vec3(0.0f, 0.0f, 0.0f) },
        
        // Dusk (18:00)
        { 18.0f, Vec3(0.2f, 0.3f, 0.6f),    Vec3(1.0f, 0.4f, 0.2f),    Vec3(1.0f, 0.5f, 0.2f), Vec3(0.3f, 0.3f, 0.4f) },
        
        // Wrap to midnight
        { 24.0f, Vec3(0.01f, 0.01f, 0.05f), Vec3(0.05f, 0.05f, 0.15f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.8f, 0.8f, 0.9f) }
    };
    
    const int numKeyframes = 5;
    
    // Find the two keyframes to interpolate between
    int i1 = 0, i2 = 1;
    for (int i = 0; i < numKeyframes - 1; i++) {
        if (m_currentTime >= keyframes[i].time && m_currentTime < keyframes[i+1].time) {
            i1 = i;
            i2 = i + 1;
            break;
        }
    }
    
    // Calculate interpolation factor (0 to 1 between keyframes)
    float t = (m_currentTime - keyframes[i1].time) / (keyframes[i2].time - keyframes[i1].time);
    t = std::clamp(t, 0.0f, 1.0f);
    
    // Smooth the interpolation with smoothstep for even smoother transitions
    t = smoothTransition(t);
    
    // Interpolate all colors
    colors.zenith = keyframes[i1].zenith.lerp(keyframes[i2].zenith, t);
    colors.horizon = keyframes[i1].horizon.lerp(keyframes[i2].horizon, t);
    colors.sunColor = keyframes[i1].sunColor.lerp(keyframes[i2].sunColor, t);
    colors.moonColor = keyframes[i1].moonColor.lerp(keyframes[i2].moonColor, t);
    
    // Fog color matches horizon
    colors.fogColor = colors.horizon;
    
    return colors;
}

DayNightController::Period DayNightController::getCurrentPeriod() const {
    if (m_currentTime >= 0.0f && m_currentTime < 5.0f) return Period::NIGHT;
    if (m_currentTime >= 5.0f && m_currentTime < 7.0f) return Period::DAWN;
    if (m_currentTime >= 7.0f && m_currentTime < 11.0f) return Period::MORNING;
    if (m_currentTime >= 11.0f && m_currentTime < 13.0f) return Period::MIDDAY;
    if (m_currentTime >= 13.0f && m_currentTime < 17.0f) return Period::AFTERNOON;
    if (m_currentTime >= 17.0f && m_currentTime < 19.0f) return Period::DUSK;
    return Period::EVENING;
}

const char* DayNightController::getPeriodName() const {
    switch (getCurrentPeriod()) {
        case Period::NIGHT: return "Night";
        case Period::DAWN: return "Dawn";
        case Period::MORNING: return "Morning";
        case Period::MIDDAY: return "Midday";
        case Period::AFTERNOON: return "Afternoon";
        case Period::DUSK: return "Dusk";
        case Period::EVENING: return "Evening";
        default: return "Unknown";
    }
}
