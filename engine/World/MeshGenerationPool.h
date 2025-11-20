// MeshGenerationPool.h - Thread pool for async mesh generation
#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

/**
 * Singleton thread pool for async mesh generation.
 * Uses a single worker thread to avoid thread spawning overhead.
 */
class MeshGenerationPool
{
public:
    static MeshGenerationPool& getInstance() {
        static MeshGenerationPool instance;
        return instance;
    }

    // Delete copy/move constructors
    MeshGenerationPool(const MeshGenerationPool&) = delete;
    MeshGenerationPool& operator=(const MeshGenerationPool&) = delete;

    // Enqueue a task to the pool
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_taskQueue.push(std::move(task));
        }
        m_condition.notify_one();
    }

    ~MeshGenerationPool() {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stop = true;
        }
        m_condition.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

private:
    MeshGenerationPool() : m_stop(false) {
        m_worker = std::thread([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_condition.wait(lock, [this]() { return m_stop || !m_taskQueue.empty(); });
                    if (m_stop && m_taskQueue.empty()) {
                        return;
                    }
                    task = std::move(m_taskQueue.front());
                    m_taskQueue.pop();
                }
                task();
            }
        });
    }

    std::thread m_worker;
    std::queue<std::function<void()>> m_taskQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop;
};
