// Profiler.cpp - Basic performance profiler implementation (moved to Profiling)
#include "Profiler.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

// Global profiler instance: provide a never-destroyed singleton to avoid
// shutdown order issues with STL internals during process teardown.
static Profiler* GetProfilerSingleton()
{
    static Profiler* s_instance = new Profiler();
    return s_instance;
}

Profiler& g_profiler = *GetProfilerSingleton();

Profiler::Profiler()
{
    m_startTime = std::chrono::high_resolution_clock::now();
}

Profiler::~Profiler()
{
    m_enabled = false;
}

void Profiler::recordTime(const std::string& name, double timeMs)
{
    if (!m_enabled)
        return;

    std::lock_guard<std::mutex> lock(m_profileMutex);
    
    ProfileData& data = m_profiles[name];
    data.name = name;
    data.totalTime += timeMs;
    data.sampleCount++;
    
    if (timeMs < data.minTime)
        data.minTime = timeMs;
    if (timeMs > data.maxTime)
        data.maxTime = timeMs;
}

const Profiler::ProfileData* Profiler::getProfileData(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_profileMutex);
    auto it = m_profiles.find(name);
    return (it != m_profiles.end()) ? &it->second : nullptr;
}

void Profiler::printShutdownReport()
{
    std::lock_guard<std::mutex> lock(m_profileMutex);
    
    if (m_profiles.empty())
        return;

    std::vector<std::pair<std::string, ProfileData*>> sortedProfiles;
    for (auto& pair : m_profiles)
    {
        if (pair.second.sampleCount > 0)
        {
            sortedProfiles.push_back({pair.first, &pair.second});
        }
    }
    
    std::sort(sortedProfiles.begin(), sortedProfiles.end(),
        [](const auto& a, const auto& b) {
            return a.second->totalTime > b.second->totalTime;
        });

    double sessionTime = getElapsedTime();
    std::cout << "\n=== PROFILER SESSION REPORT (Runtime: " << std::fixed << std::setprecision(1) 
              << sessionTime << "s) ===" << std::endl;
    
    // Filter out insignificant entries (< 1ms total or < 0.01ms avg)
    std::vector<std::pair<std::string, ProfileData*>> significantProfiles;
    for (const auto& entry : sortedProfiles)
    {
        const ProfileData& data = *entry.second;
        if (data.totalTime >= 1.0 && data.getAverageTime() >= 0.01)
        {
            significantProfiles.push_back(entry);
        }
    }
    
    if (significantProfiles.empty())
    {
        std::cout << "No significant profiles recorded." << std::endl;
        return;
    }
    
    // Calculate percentage of total time
    double totalRecordedTime = 0.0;
    for (const auto& entry : significantProfiles)
    {
        totalRecordedTime += entry.second->totalTime;
    }
    
    std::cout << std::left << std::setw(35) << "Function"
              << std::right << std::setw(10) << "Total(ms)"
              << std::setw(10) << "Avg(ms)"
              << std::setw(10) << "Max(ms)"
              << std::setw(8) << "% Time"
              << std::setw(10) << "Calls" << std::endl;
    std::cout << std::string(83, '-') << std::endl;

    // Report significant profiles only
    for (const auto& entry : significantProfiles)
    {
        const ProfileData& data = *entry.second;
        double percentOfTotal = (data.totalTime / totalRecordedTime) * 100.0;
        
        std::cout << std::left << std::setw(35) << data.name.substr(0, 34)
                  << std::right << std::setw(10) << std::fixed << std::setprecision(2) << data.totalTime
                  << std::setw(10) << std::fixed << std::setprecision(2) << data.getAverageTime()
                  << std::setw(10) << std::fixed << std::setprecision(2) << data.maxTime
                  << std::setw(7) << std::fixed << std::setprecision(1) << percentOfTotal << "%"
                  << std::setw(10) << data.sampleCount
                  << std::endl;
    }
    
    std::cout << "\nFiltered out " << (sortedProfiles.size() - significantProfiles.size()) 
              << " insignificant entries (< 1ms total)" << std::endl;
    std::cout << std::endl;
}

double Profiler::getElapsedTime() const
{
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(currentTime - m_startTime);
    return duration.count();
}

// ProfileScope implementation
ProfileScope::ProfileScope(const std::string& name)
    : m_name(name), m_active(true)
{
    m_startTime = std::chrono::high_resolution_clock::now();
}

ProfileScope::~ProfileScope()
{
    stop();
}

void ProfileScope::stop()
{
    if (!m_active)
        return;

    // Check if profiler is still enabled (avoid recording during shutdown)
    if (!g_profiler.isEnabled())
    {
        m_active = false;
        return;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(endTime - m_startTime);
    
    g_profiler.recordTime(m_name, duration.count());
    m_active = false;
}
