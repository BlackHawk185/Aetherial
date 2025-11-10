// Profiler.h - Basic performance profiler for MMORPG Engine
// (Moved from Core to Profiling)
#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

class Profiler
{
public:
    struct ProfileData
    {
        std::string name;
        double totalTime = 0.0;
        double minTime = 99999.0;
        double maxTime = 0.0;
        uint32_t sampleCount = 0;
        
        double getAverageTime() const
        {
            return sampleCount > 0 ? totalTime / sampleCount : 0.0;
        }
    };

private:
    std::unordered_map<std::string, ProfileData> m_profiles;
    std::chrono::high_resolution_clock::time_point m_startTime;
    mutable std::mutex m_profileMutex;
    bool m_enabled = true;

public:
    Profiler();
    ~Profiler();

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void recordTime(const std::string& name, double timeMs);
    void printShutdownReport();
    const ProfileData* getProfileData(const std::string& name) const;

private:
    double getElapsedTime() const;
};

// RAII profiler scope for automatic timing
class ProfileScope
{
private:
    std::string m_name;
    std::chrono::high_resolution_clock::time_point m_startTime;
    bool m_active;

public:
    ProfileScope(const std::string& name);
    ~ProfileScope();

    // Manual stop (optional - destructor will call this)
    void stop();
};

// Global profiler instance (never destroyed; avoids shutdown-order issues)
extern Profiler& g_profiler;

// Convenient macros for profiling
#define PROFILE_SCOPE(name) ProfileScope _prof_scope(name)
#define PROFILE_FUNCTION() ProfileScope _prof_scope(__FUNCTION__)
