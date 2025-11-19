// GPUProfiler.cpp - OpenGL GPU timer query profiler implementation
#include "GPUProfiler.h"
#include "Profiler.h"
#include <iostream>

// Global GPU profiler instance
static GPUProfiler* GetGPUProfilerSingleton()
{
    static GPUProfiler* s_instance = new GPUProfiler();
    return s_instance;
}

GPUProfiler& g_gpuProfiler = *GetGPUProfilerSingleton();

GPUProfiler::GPUProfiler()
{
}

GPUProfiler::~GPUProfiler()
{
    cleanup();
}

void GPUProfiler::beginQuery(const std::string& name)
{
    if (!m_enabled)
        return;

    GPUQuery& query = m_queries[name];
    
    // Create queries if they don't exist
    if (query.queryStart == 0)
    {
        glGenQueries(1, &query.queryStart);
        glGenQueries(1, &query.queryEnd);
        query.name = name;
    }

    // Issue the start query
    glQueryCounter(query.queryStart, GL_TIMESTAMP);
    query.active = true;
    m_activeStack.push_back(name);
}

void GPUProfiler::endQuery(const std::string& name)
{
    if (!m_enabled)
        return;

    auto it = m_queries.find(name);
    if (it == m_queries.end() || !it->second.active)
        return;

    GPUQuery& query = it->second;
    
    // Issue the end query
    glQueryCounter(query.queryEnd, GL_TIMESTAMP);
    query.active = false;

    // Remove from active stack
    if (!m_activeStack.empty() && m_activeStack.back() == name)
        m_activeStack.pop_back();
}

void GPUProfiler::collectResults()
{
    if (!m_enabled)
        return;

    for (auto& pair : m_queries)
    {
        GPUQuery& query = pair.second;
        
        if (query.queryStart == 0 || query.queryEnd == 0)
            continue;

        // Check if results are available
        GLint startAvailable = 0;
        GLint endAvailable = 0;
        glGetQueryObjectiv(query.queryStart, GL_QUERY_RESULT_AVAILABLE, &startAvailable);
        glGetQueryObjectiv(query.queryEnd, GL_QUERY_RESULT_AVAILABLE, &endAvailable);

        if (startAvailable && endAvailable)
        {
            GLuint64 startTime, endTime;
            glGetQueryObjectui64v(query.queryStart, GL_QUERY_RESULT, &startTime);
            glGetQueryObjectui64v(query.queryEnd, GL_QUERY_RESULT, &endTime);

            // Convert nanoseconds to milliseconds
            double timeMs = (endTime - startTime) / 1000000.0;
            
            // Record to the global profiler
            g_profiler.recordGPUTime(query.name, timeMs);
        }
    }
}

void GPUProfiler::cleanup()
{
    for (auto& pair : m_queries)
    {
        GPUQuery& query = pair.second;
        if (query.queryStart != 0)
            glDeleteQueries(1, &query.queryStart);
        if (query.queryEnd != 0)
            glDeleteQueries(1, &query.queryEnd);
    }
    m_queries.clear();
    m_activeStack.clear();
}

// GPUProfileScope implementation
GPUProfileScope::GPUProfileScope(const std::string& name)
    : m_name(name), m_active(true)
{
    g_gpuProfiler.beginQuery(m_name);
}

GPUProfileScope::~GPUProfileScope()
{
    if (m_active)
    {
        g_gpuProfiler.endQuery(m_name);
        m_active = false;
    }
}
