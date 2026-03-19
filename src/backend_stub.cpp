#include "RunLoop.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace ms
{

    RunLoop::RunLoop() = default;

    RunLoop::~RunLoop()
    {
        if (m_running.load())
        {
            stop();
        }

        if (m_wakeupFd[0] >= 0)
        {
            m_wakeupFd[0] = m_wakeupFd[1] = -1;
        }
    }

    void RunLoop::init(const char *name)
    {
        if (m_wakeupFd[0] >= 0)
        {
            throw std::logic_error("RunLoop::init: already initialized");
        }

        m_name = name ? name : "";
        // Use sentinel values to indicate initialized state.
        m_wakeupFd[0] = 0;
        m_wakeupFd[1] = 0;
    }

    void RunLoop::run()
    {
        m_running.store(true, std::memory_order_release);

        auto resetFlags = [this]()
        {
            m_running.store(false, std::memory_order_release);
            m_stopRequested.store(false, std::memory_order_release);
        };

        try
        {
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                // Execute posted callables
                {
                    std::vector<std::function<void()>> batch;
                    {
                        std::lock_guard<std::mutex> lock(m_postMutex);
                        batch.swap(m_postQueue);
                    }
                    for (auto &fn : batch)
                    {
                        fn();
                    }
                }

                // No OS polling mechanism — sleep briefly to avoid busy-spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        catch (...)
        {
            resetFlags();
            throw;
        }

        resetFlags();
    }

    void RunLoop::stop()
    {
        m_stopRequested.store(true, std::memory_order_release);
        wakeup();
    }

    void RunLoop::executeOnRunLoop(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(m_postMutex);
            m_postQueue.push_back(std::move(fn));
        }
        wakeup();
    }

    void RunLoop::addSource(NativeHandle handle, std::function<void()> handler)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources[handle] = std::move(handler);
        // Stub: handler is stored but no OS-level monitoring is performed.
        // Source handlers will not fire until a platform-specific backend
        // is implemented for the target RTOS.
    }

    void RunLoop::removeSource(NativeHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources.erase(handle);
    }

    void RunLoop::wakeup()
    {
        // No-op: the stub backend polls m_stopRequested via sleep loop.
    }

} // namespace ms
