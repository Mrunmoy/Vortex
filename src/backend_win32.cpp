#include "RunLoop.h"

#include <cassert>
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

    // Max usable WFMO slots: 1 slot reserved for wakeup event, remainder
    // shared by sources and timers.  MAXIMUM_WAIT_OBJECTS is 64 on all
    // Windows SKUs.
    static constexpr DWORD kMaxWaitSlots = MAXIMUM_WAIT_OBJECTS - 1;

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
                {
                    // Atomic snapshot: both locks held to prevent WFMO overflow.
                    // Lock ordering: m_sourcesMutex before m_timersMutex.
                    std::lock_guard<std::mutex> slock(m_sourcesMutex);
                    std::lock_guard<std::mutex> tlock(m_timersMutex);
                    handles.reserve(1 + m_sources.size() + m_timers.size());
                    handles.push_back(m_wakeupHandle);
                    for (auto &[h, _] : m_sources)
                    {
                        handles.push_back(static_cast<HANDLE>(h));
                    }
                    for (auto &[tid, entry] : m_timers)
                    {
                        if (entry.handle != nullptr)
                        {
                            handles.push_back(entry.handle);
                        }
                    }
                }

                assert(handles.size() <= MAXIMUM_WAIT_OBJECTS);

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
                        // Dispatch helper: look up and invoke the handler for a
                        // signaled handle (timer or source).
                        auto dispatchHandle = [this](HANDLE signalled) {
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
                                // Source dispatch.
                                // NOTE: onError callbacks are stored in SourceEntry
                                // but never fired by the WFMO backend — WFMO cannot
                                // distinguish data-ready from error/hangup.  Only
                                // the data handler is invoked here.  Error-callback
                                // parity requires the planned IOCP backend.
                                {
                                    std::lock_guard<std::mutex> lock(m_sourcesMutex);
                                    auto it = m_sources.find(signalled);
                                    if (it != m_sources.end())
                                    {
                                        handler = it->second.handler;
                                    }
                                }
                                if (handler)
                                {
                                    handler();
                                }
                            }
                        };

                        // Dispatch the primary signaled handle.
                        dispatchHandle(handles[idx]);

                        // Starvation sweep: WFMO always returns the lowest
                        // signaled index, starving higher-indexed handles under
                        // load.  Poll remaining handles with a zero timeout to
                        // give them a chance to dispatch in the same iteration.
                        // This is a mitigation, not a full fix — true fairness
                        // requires the planned IOCP backend.
                        for (DWORD i = idx + 1; i < count; ++i)
                        {
                            if (m_stopRequested.load(std::memory_order_acquire))
                            {
                                break;
                            }
                            if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                            {
                                dispatchHandle(handles[i]);
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
        addSource(handle, std::move(handler), nullptr);
    }

    void RunLoop::addSource(NativeHandle handle, std::function<void()> handler,
                            std::function<void()> onError)
    {
        // Lock ordering: m_sourcesMutex before m_timersMutex.
        std::lock_guard<std::mutex> slock(m_sourcesMutex);
        std::lock_guard<std::mutex> tlock(m_timersMutex);

        const bool isNew = (m_sources.find(handle) == m_sources.end());
        if (isNew && (m_sources.size() + m_timers.size() >= kMaxWaitSlots))
        {
            throw std::runtime_error(
                "RunLoop::addSource: slot limit reached (sources="
                + std::to_string(m_sources.size())
                + ", timers=" + std::to_string(m_timers.size())
                + ", max=" + std::to_string(kMaxWaitSlots) + ")");
        }

        m_sources[handle] = {std::move(handler), std::move(onError)};
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
            // Lock ordering: m_sourcesMutex before m_timersMutex.
            std::lock_guard<std::mutex> slock(m_sourcesMutex);
            std::lock_guard<std::mutex> tlock(m_timersMutex);
            if (m_sources.size() + m_timers.size() >= kMaxWaitSlots)
            {
                CancelWaitableTimer(timerHandle);
                CloseHandle(timerHandle);
                throw std::runtime_error(
                    "RunLoop::addTimer: slot limit reached (sources="
                    + std::to_string(m_sources.size())
                    + ", timers=" + std::to_string(m_timers.size())
                    + ", max=" + std::to_string(kMaxWaitSlots) + ")");
            }
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
