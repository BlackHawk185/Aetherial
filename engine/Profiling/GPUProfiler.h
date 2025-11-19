// GPUProfiler.h - OpenGL GPU timer query profiler
#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>
#include <unordered_map>

class GPUProfiler
{
public:
    struct GPUQuery
    {
        GLuint queryStart = 0;
        GLuint queryEnd = 0;
        std::string name;
        bool active = false;
    };

private:
    std::unordered_map<std::string, GPUQuery> m_queries;
    std::vector<std::string> m_activeStack;
    bool m_enabled = true;

public:
    GPUProfiler();
    ~GPUProfiler();

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void beginQuery(const std::string& name);
    void endQuery(const std::string& name);

    void collectResults();
    void cleanup();
};

// RAII GPU profiler scope for automatic timing
class GPUProfileScope
{
private:
    std::string m_name;
    bool m_active;

public:
    GPUProfileScope(const std::string& name);
    ~GPUProfileScope();
};

// Global GPU profiler instance
extern GPUProfiler& g_gpuProfiler;

// Convenient macro for GPU profiling
#define GPU_PROFILE_SCOPE(name) GPUProfileScope _gpu_prof_scope(name)
