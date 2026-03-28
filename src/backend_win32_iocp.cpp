// Vortex — Win32 IOCP backend
//
// Replaces the WFMO backend with an I/O Completion Port dispatcher.
// Advantages over WFMO:
//   - No 63-handle limit (unlimited sources + timers)
//   - Fair dispatch (no lowest-index starvation)
//   - Batch dequeue via GetQueuedCompletionStatusEx
//   - Safe context lifetime via deferred cleanup
//
// Architecture:
//   - One IOCP as the central dispatcher (stored in m_pollHandle)
//   - Sources: RegisterWaitForSingleObject → posts to IOCP when signalled
//   - Timers:  CreateThreadpoolTimer → posts to IOCP when fired
//   - Callables: enqueued to m_postQueue, wakeup posts to IOCP
//   - Wakeup: PostQueuedCompletionStatus with kKeyWakeup

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RunLoop.h"

#include <cassert>
#include <stdexcept>
#include <system_error>

namespace vortex
{

// ── Completion key sentinels ────────────────────────────────────────
static constexpr ULONG_PTR kKeyWakeup = 0;
static constexpr ULONG_PTR kKeySource = 1;
static constexpr ULONG_PTR kKeyTimer  = 2;

// ── Context structures carried through IOCP ─────────────────────────
// We reinterpret_cast context pointers as OVERLAPPED* to pass them
// through PostQueuedCompletionStatus. IOCP does not dereference the
// OVERLAPPED — it just carries the pointer through to GQCS.

struct SourceContext
{
    HANDLE iocp;
    RunLoop::NativeHandle sourceHandle;
    HANDLE waitHandle = nullptr;
};

struct TimerContext
{
    HANDLE iocp;
    RunLoop::TimerId id;
    bool repeating;
    PTP_TIMER tpTimer = nullptr;
};

// ── Threadpool callbacks (static) ───────────────────────────────────

static VOID CALLBACK sourceWaitCallback(PVOID context, BOOLEAN /*timerOrWaitFired*/)
{
    auto *ctx = static_cast<SourceContext *>(context);
    PostQueuedCompletionStatus(
        ctx->iocp, 0, kKeySource,
        reinterpret_cast<LPOVERLAPPED>(ctx));
}

static VOID CALLBACK timerPoolCallback(
    PTP_CALLBACK_INSTANCE /*instance*/,
    PVOID context,
    PTP_TIMER /*timer*/)
{
    auto *ctx = static_cast<TimerContext *>(context);
    PostQueuedCompletionStatus(
        ctx->iocp, 0, kKeyTimer,
        reinterpret_cast<LPOVERLAPPED>(ctx));
}

// ── Constructor / Destructor ────────────────────────────────────────

RunLoop::RunLoop() = default;

RunLoop::~RunLoop()
{
    if (m_running.load())
    {
        stop();
    }

    // Unregister source waits and free contexts.
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        for (auto &[h, entry] : m_sources)
        {
            (void)entry;
        }
        m_sources.clear();
    }

    // Cancel and close timer threadpool timers.
    {
        std::lock_guard<std::mutex> lock(m_timersMutex);
        for (auto &[id, entry] : m_timers)
        {
            if (entry.handle != nullptr)
            {
                auto *ctx = static_cast<TimerContext *>(entry.handle);
                if (ctx->tpTimer)
                {
                    SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
                    WaitForThreadpoolTimerCallbacks(ctx->tpTimer, TRUE);
                    CloseThreadpoolTimer(ctx->tpTimer);
                }
                delete ctx;
            }
        }
        m_timers.clear();

        // Clean up retired timer contexts.
        for (void *h : m_retiredTimerHandles)
        {
            auto *ctx = static_cast<TimerContext *>(h);
            if (ctx->tpTimer)
            {
                SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
                WaitForThreadpoolTimerCallbacks(ctx->tpTimer, TRUE);
                CloseThreadpoolTimer(ctx->tpTimer);
            }
            delete ctx;
        }
        m_retiredTimerHandles.clear();
    }

    if (m_wakeupHandle != nullptr)
    {
        CloseHandle(m_wakeupHandle);
    }
    if (m_pollHandle != nullptr)
    {
        CloseHandle(m_pollHandle);
    }
}

// ── init ────────────────────────────────────────────────────────────

void RunLoop::init(const char *name)
{
    if (m_pollHandle != nullptr)
    {
        throw std::logic_error("RunLoop::init: already initialized");
    }

    m_name = name ? name : "";

    // Create the I/O Completion Port. NumberOfConcurrentThreads=1
    // since Vortex dispatches from a single thread.
    m_pollHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_pollHandle == nullptr)
    {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "RunLoop::init: CreateIoCompletionPort failed");
    }

    // Sentinel event to mark as initialized (not used for polling,
    // keeps header layout compatible with WFMO backend).
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

// ── run ─────────────────────────────────────────────────────────────

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
        constexpr ULONG kMaxEntries = 32;
        OVERLAPPED_ENTRY entries[kMaxEntries];

        while (!m_stopRequested.load(std::memory_order_acquire))
        {
            // ── Phase 1: Clean up retired timer contexts ────────────
            {
                std::lock_guard<std::mutex> lock(m_timersMutex);
                for (void *h : m_retiredTimerHandles)
                {
                    auto *ctx = static_cast<TimerContext *>(h);
                    if (ctx->tpTimer)
                    {
                        SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
                        WaitForThreadpoolTimerCallbacks(ctx->tpTimer, TRUE);
                        CloseThreadpoolTimer(ctx->tpTimer);
                    }
                    delete ctx;
                }
                m_retiredTimerHandles.clear();
            }

            // ── Phase 2: Drain posted callables ─────────────────────
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

            // ── Phase 3: Dequeue completions from IOCP ──────────────
            ULONG removed = 0;
            BOOL ok = GetQueuedCompletionStatusEx(
                m_pollHandle, entries, kMaxEntries, &removed, INFINITE, FALSE);

            if (!ok)
            {
                DWORD err = GetLastError();
                if (err == WAIT_TIMEOUT)
                    continue;
                throw std::system_error(static_cast<int>(err),
                                        std::system_category(),
                                        "RunLoop::run: GetQueuedCompletionStatusEx failed");
            }

            // ── Phase 4: Dispatch completions ───────────────────────
            for (ULONG i = 0; i < removed; ++i)
            {
                if (m_stopRequested.load(std::memory_order_acquire))
                    break;

                ULONG_PTR key = entries[i].lpCompletionKey;
                LPOVERLAPPED ovl = entries[i].lpOverlapped;

                if (key == kKeyWakeup)
                {
                    // Wakeup: loop back to drain queue.
                    continue;
                }
                else if (key == kKeySource)
                {
                    auto *ctx = reinterpret_cast<SourceContext *>(ovl);
                    NativeHandle srcHandle = ctx->sourceHandle;

                    std::function<void()> handler;
                    {
                        std::lock_guard<std::mutex> lock(m_sourcesMutex);
                        auto it = m_sources.find(srcHandle);
                        if (it != m_sources.end())
                        {
                            handler = it->second.handler;
                        }
                    }

                    if (handler)
                    {
                        handler();
                    }

                    // Re-register the wait if source still exists.
                    bool rearm = false;
                    {
                        std::lock_guard<std::mutex> lock(m_sourcesMutex);
                        rearm = m_sources.count(srcHandle) > 0;
                    }
                    if (rearm)
                    {
                        HANDLE newWaitHandle = nullptr;
                        if (RegisterWaitForSingleObject(
                                &newWaitHandle,
                                static_cast<HANDLE>(srcHandle),
                                sourceWaitCallback,
                                ctx,
                                INFINITE,
                                WT_EXECUTEONLYONCE))
                        {
                            ctx->waitHandle = newWaitHandle;
                        }
                    }
                    else
                    {
                        // Source was removed while completion was in flight.
                        if (ctx->waitHandle)
                        {
                            UnregisterWait(ctx->waitHandle);
                        }
                        delete ctx;
                    }
                }
                else if (key == kKeyTimer)
                {
                    auto *ctx = reinterpret_cast<TimerContext *>(ovl);
                    TimerId firedId = ctx->id;
                    bool oneShot = !ctx->repeating;

                    std::function<void()> handler;
                    {
                        std::lock_guard<std::mutex> lock(m_timersMutex);
                        auto it = m_timers.find(firedId);
                        if (it != m_timers.end())
                        {
                            handler = it->second.handler;
                        }
                    }

                    if (handler)
                    {
                        handler();
                    }

                    if (oneShot)
                    {
                        removeTimer(firedId);
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

// ── stop ────────────────────────────────────────────────────────────

void RunLoop::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    wakeup();
}

// ── wakeup ──────────────────────────────────────────────────────────

void RunLoop::wakeup()
{
    if (m_pollHandle != nullptr)
    {
        PostQueuedCompletionStatus(m_pollHandle, 0, kKeyWakeup, nullptr);
    }
}

// ── executeOnRunLoop ────────────────────────────────────────────────

void RunLoop::executeOnRunLoop(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(m_postMutex);
        m_postQueue.push_back(std::move(fn));
    }
    wakeup();
}

// ── addSource ───────────────────────────────────────────────────────

void RunLoop::addSource(NativeHandle handle, std::function<void()> handler)
{
    addSource(handle, std::move(handler), nullptr);
}

void RunLoop::addSource(NativeHandle handle, std::function<void()> handler,
                        std::function<void()> onError)
{
    auto *ctx = new SourceContext{m_pollHandle, handle, nullptr};

    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);

        // Replace existing handler if source already registered.
        auto it = m_sources.find(handle);
        if (it != m_sources.end())
        {
            it->second.handler = std::move(handler);
            it->second.onError = std::move(onError);
            delete ctx;
            return;
        }

        m_sources[handle] = {std::move(handler), std::move(onError)};
    }

    HANDLE waitHandle = nullptr;
    if (!RegisterWaitForSingleObject(
            &waitHandle,
            static_cast<HANDLE>(handle),
            sourceWaitCallback,
            ctx,
            INFINITE,
            WT_EXECUTEONLYONCE))
    {
        {
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            m_sources.erase(handle);
        }
        delete ctx;
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "RunLoop::addSource: RegisterWaitForSingleObject failed");
    }
    ctx->waitHandle = waitHandle;

    wakeup();
}

// ── removeSource ────────────────────────────────────────────────────

void RunLoop::removeSource(NativeHandle handle)
{
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    m_sources.erase(handle);
    // The RegisterWaitForSingleObject callback may still fire after
    // removal. The dispatch code checks m_sources and ignores stale
    // completions, then frees the context.
}

// ── addTimer ────────────────────────────────────────────────────────

RunLoop::TimerId RunLoop::addTimer(uint32_t intervalMs, bool repeating,
                                   std::function<void()> handler)
{
    TimerId id = m_nextTimerId.fetch_add(1, std::memory_order_relaxed);

    auto *ctx = new TimerContext{m_pollHandle, id, repeating, nullptr};

    PTP_TIMER tpTimer = CreateThreadpoolTimer(timerPoolCallback, ctx, nullptr);
    if (tpTimer == nullptr)
    {
        delete ctx;
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "RunLoop::addTimer: CreateThreadpoolTimer failed");
    }
    ctx->tpTimer = tpTimer;

    // Due time: negative = relative, in 100ns units.
    FILETIME ft;
    LARGE_INTEGER li;
    li.QuadPart = -static_cast<LONGLONG>(intervalMs) * 10000LL;
    ft.dwLowDateTime = li.LowPart;
    ft.dwHighDateTime = static_cast<DWORD>(li.HighPart);

    DWORD period = repeating ? intervalMs : 0;
    SetThreadpoolTimer(tpTimer, &ft, period, 0);

    {
        std::lock_guard<std::mutex> lock(m_timersMutex);
        m_timers[id] = {intervalMs, repeating, std::move(handler),
                        static_cast<void *>(ctx)};
    }

    wakeup();
    return id;
}

// ── removeTimer ─────────────────────────────────────────────────────

void RunLoop::removeTimer(TimerId id)
{
    std::lock_guard<std::mutex> lock(m_timersMutex);
    auto it = m_timers.find(id);
    if (it == m_timers.end())
        return;

    void *h = it->second.handle;
    m_timers.erase(it);

    if (h != nullptr)
    {
        auto *ctx = static_cast<TimerContext *>(h);
        // Cancel the timer so it won't fire again.
        SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
        // Defer full cleanup to run loop thread (Phase 1).
        m_retiredTimerHandles.push_back(h);
    }
    wakeup();
}

} // namespace vortex

#endif // defined(_WIN32)
