#include "RunLoop.h"

#include <stdexcept>
#include <system_error>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vortex
{

    // Win32 backend notes:
    // - Sources must be waitable kernel objects (events, semaphores, mutexes,
    //   processes, threads). Pipes and sockets are NOT waitable for readability
    //   via WaitForMultipleObjects — use overlapped I/O for those.
    // - Maximum source count is MAXIMUM_WAIT_OBJECTS - 1 (63), since the
    //   wakeup event occupies one slot.

    static constexpr DWORD MAX_SOURCES = MAXIMUM_WAIT_OBJECTS - 1;

    RunLoop::RunLoop() = default;

    RunLoop::~RunLoop()
    {
        if (m_running.load())
        {
            stop();
        }

        // Close timer handles.
        for (auto &[id, entry] : m_timers)
        {
            if (entry.handle != nullptr)
            {
                CloseHandle(entry.handle);
            }
        }
        for (HANDLE h : m_retiredTimerHandles)
            CloseHandle(h);
        m_retiredTimerHandles.clear();

        if (m_wakeupHandle != nullptr)
        {
            CloseHandle(m_wakeupHandle);
        }
        if (m_pollHandle != nullptr)
        {
            CloseHandle(m_pollHandle);
        }
    }

    void RunLoop::init(const char *name)
    {
        if (m_pollHandle != nullptr)
        {
            throw std::logic_error("RunLoop::init: already initialized");
        }

        m_name = name ? name : "";

        // Sentinel handle to mark as initialized (not used for polling).
        m_pollHandle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (m_pollHandle == nullptr)
        {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "RunLoop::init: CreateEvent (poll) failed");
        }

        // Manual-reset event for wakeup signalling.
        m_wakeupHandle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (m_wakeupHandle == nullptr)
        {
            CloseHandle(m_pollHandle);
            m_pollHandle = nullptr;
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "RunLoop::init: CreateEvent (wakeup) failed");
        }
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
                // Close timer handles that were retired by removeTimer().
                {
                    std::lock_guard<std::mutex> lock(m_timersMutex);
                    for (HANDLE h : m_retiredTimerHandles)
                        CloseHandle(h);
                    m_retiredTimerHandles.clear();
                }

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

                // Build wait array: wakeup event + source handles + timer handles.
                std::vector<HANDLE> handles;
                std::vector<TimerId> timerIds;
                handles.push_back(m_wakeupHandle);
                {
                    std::lock_guard<std::mutex> lock(m_sourcesMutex);
                    for (auto &[h, _] : m_sources)
                    {
                        handles.push_back(static_cast<HANDLE>(h));
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(m_timersMutex);
                    for (auto &[tid, entry] : m_timers)
                    {
                        if (entry.handle != nullptr)
                        {
                            handles.push_back(entry.handle);
                            timerIds.push_back(tid);
                        }
                    }
                }

                DWORD count = static_cast<DWORD>(handles.size());
                DWORD result = WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);

                if (result == WAIT_FAILED)
                {
                    throw std::system_error(static_cast<int>(GetLastError()),
                                            std::system_category(),
                                            "RunLoop::run: WaitForMultipleObjects failed");
                }

                if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
                {
                    DWORD idx = result - WAIT_OBJECT_0;
                    if (idx == 0)
                    {
                        // Wakeup event signalled — reset it and loop back to drain queue.
                        ResetEvent(m_wakeupHandle);
                    }
                    else
                    {
                        HANDLE signalled = handles[idx];

                        // Check if it's a timer handle.
                        std::function<void()> handler;
                        bool isTimer = false;
                        TimerId firedTimerId = 0;
                        bool oneShot = false;
                        {
                            std::lock_guard<std::mutex> lock(m_timersMutex);
                            for (auto &[tid, entry] : m_timers)
                            {
                                if (entry.handle == signalled)
                                {
                                    isTimer = true;
                                    firedTimerId = tid;
                                    oneShot = !entry.repeating;
                                    handler = entry.handler;
                                    break;
                                }
                            }
                        }

                        if (isTimer)
                        {
                            if (handler)
                            {
                                handler();
                            }
                            if (oneShot)
                            {
                                removeTimer(firedTimerId);
                            }
                        }
                        else
                        {
                            {
                                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                                auto it = m_sources.find(signalled);
                                if (it != m_sources.end())
                                {
                                    handler = it->second;
                                }
                            }
                            if (handler)
                            {
                                handler();
                            }
                        }
                    }
                }
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
        if (m_sources.find(handle) == m_sources.end() && m_sources.size() >= MAX_SOURCES)
        {
            throw std::runtime_error(
                "RunLoop::addSource: source limit reached (max "
                + std::to_string(MAX_SOURCES) + ")");
        }
        m_sources[handle] = std::move(handler);
        wakeup();
    }

    void RunLoop::removeSource(NativeHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources.erase(handle);
        wakeup();
    }

    void RunLoop::wakeup()
    {
        if (m_wakeupHandle != nullptr)
        {
            SetEvent(m_wakeupHandle);
        }
    }

    RunLoop::TimerId RunLoop::addTimer(uint32_t intervalMs, bool repeating,
                                       std::function<void()> handler)
    {
        HANDLE timerHandle = CreateWaitableTimerA(nullptr, FALSE, nullptr);
        if (timerHandle == nullptr)
        {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "RunLoop::addTimer: CreateWaitableTimer failed");
        }

        // Due time is negative for relative time (in 100ns intervals).
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -static_cast<LONGLONG>(intervalMs) * 10000LL;
        LONG period = repeating ? static_cast<LONG>(intervalMs) : 0;

        if (!SetWaitableTimer(timerHandle, &dueTime, period, nullptr, nullptr, FALSE))
        {
            DWORD err = GetLastError();
            CloseHandle(timerHandle);
            throw std::system_error(static_cast<int>(err),
                                    std::system_category(),
                                    "RunLoop::addTimer: SetWaitableTimer failed");
        }

        TimerId id = m_nextTimerId.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_timersMutex);
            m_timers[id] = {intervalMs, repeating, std::move(handler), timerHandle};
        }
        wakeup();
        return id;
    }

    void RunLoop::removeTimer(TimerId id)
    {
        std::lock_guard<std::mutex> lock(m_timersMutex);
        auto it = m_timers.find(id);
        if (it == m_timers.end())
            return;

        HANDLE h = it->second.handle;
        m_timers.erase(it);

        if (h != nullptr)
        {
            CancelWaitableTimer(h);
            // Defer CloseHandle — the run loop may still reference the
            // handle inside WaitForMultipleObjects. The run loop drains
            // m_retiredTimerHandles after the wait returns.
            m_retiredTimerHandles.push_back(h);
        }
        wakeup();
    }

} // namespace vortex

#endif // _WIN32
