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
//
// Context lifetime:
//   SourceContext* is stored in SourceEntry.context. Ownership transfers
//   to m_retiredSourceContexts on removeSource, freed in Phase 1.
//   TimerContext* is stored in TimerEntry.handle. On removeTimer, the
//   threadpool timer is cancelled and the context moves to
//   m_retiredTimerHandles. Phase 1 closes the TP resource but keeps
//   the allocation alive. Stale completions in Phase 4 delete the
//   context; the destructor frees any remaining.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RunLoop.h"

#include <algorithm>
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
// Reinterpret_cast as OVERLAPPED* to carry through IOCP. The kernel
// does not dereference the pointer — it passes it through to GQCS.

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

// ── Helper: close a timer context's TP resource ─────────────────────
static void closeTimerResource(TimerContext *ctx)
{
    if (ctx->tpTimer)
    {
        SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(ctx->tpTimer, TRUE);
        CloseThreadpoolTimer(ctx->tpTimer);
        ctx->tpTimer = nullptr;
    }
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
            if (entry.context != nullptr)
            {
                auto *ctx = static_cast<SourceContext *>(entry.context);
                if (ctx->waitHandle)
                {
                    UnregisterWaitEx(ctx->waitHandle, INVALID_HANDLE_VALUE);
                }
                delete ctx;
            }
        }
        m_sources.clear();

        // Free retired source contexts.
        for (void *p : m_retiredSourceContexts)
        {
            delete static_cast<SourceContext *>(p);
        }
        m_retiredSourceContexts.clear();
    }

    // Cancel and close timer threadpool timers.
    {
        std::lock_guard<std::mutex> lock(m_timersMutex);
        for (auto &[id, entry] : m_timers)
        {
            if (entry.handle != nullptr)
            {
                auto *ctx = static_cast<TimerContext *>(entry.handle);
                closeTimerResource(ctx);
                delete ctx;
            }
        }
        m_timers.clear();

        // Free retired timer contexts.
        for (void *h : m_retiredTimerHandles)
        {
            auto *ctx = static_cast<TimerContext *>(h);
            closeTimerResource(ctx);
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

    // NumberOfConcurrentThreads=1: Vortex dispatches from a single thread.
    m_pollHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_pollHandle == nullptr)
    {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "RunLoop::init: CreateIoCompletionPort failed");
    }

    // Sentinel event for header compatibility with WFMO backend.
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
            // ── Phase 1: Retire timer TP resources & free source contexts
            {
                std::lock_guard<std::mutex> lock(m_timersMutex);
                for (void *h : m_retiredTimerHandles)
                {
                    // Close the TP resource but keep the allocation alive.
                    // A stale IOCP completion may still reference the
                    // TimerContext*. Phase 4 frees it when consumed.
                    closeTimerResource(static_cast<TimerContext *>(h));
                }
                // Don't clear — entries move to a "zombie" state.
                // Phase 4 will erase + delete individual entries.
            }
            {
                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                for (void *p : m_retiredSourceContexts)
                {
                    delete static_cast<SourceContext *>(p);
                }
                m_retiredSourceContexts.clear();
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
                    continue;
                }
                else if (key == kKeySource)
                {
                    dispatchSource(reinterpret_cast<SourceContext *>(ovl));
                }
                else if (key == kKeyTimer)
                {
                    dispatchTimer(reinterpret_cast<TimerContext *>(ovl));
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

        auto it = m_sources.find(handle);
        if (it != m_sources.end())
        {
            // Replace handler but keep existing context/registration.
            it->second.handler = std::move(handler);
            it->second.onError = std::move(onError);
            delete ctx;
            return;
        }

        m_sources[handle] = {std::move(handler), std::move(onError), ctx};
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
    auto it = m_sources.find(handle);
    if (it == m_sources.end())
        return;

    void *rawCtx = it->second.context;
    m_sources.erase(it);

    if (rawCtx != nullptr)
    {
        auto *ctx = static_cast<SourceContext *>(rawCtx);
        if (ctx->waitHandle)
        {
            // Non-blocking unregister. The callback may still post a
            // stale IOCP completion; dispatch ignores it (source gone
            // from map) and frees the context there.
            UnregisterWait(ctx->waitHandle);
            ctx->waitHandle = nullptr;
        }
        // Defer delete — the callback might still be running on the
        // threadpool and accessing ctx->iocp right now.
        m_retiredSourceContexts.push_back(rawCtx);
    }
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
        // Cancel future firings. TP resource is closed in Phase 1;
        // the allocation lives until the stale completion is consumed.
        SetThreadpoolTimer(ctx->tpTimer, nullptr, 0, 0);
        m_retiredTimerHandles.push_back(h);
    }
    wakeup();
}

// ── Private dispatch helpers (called from run loop thread) ──────────

void RunLoop::dispatchSource(void *rawCtx)
{
    auto *ctx = static_cast<SourceContext *>(rawCtx);
    NativeHandle srcHandle = ctx->sourceHandle;

    // Look up handler and verify context identity.
    std::function<void()> handler;
    bool isCurrentCtx = false;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(srcHandle);
        if (it != m_sources.end() && it->second.context == ctx)
        {
            handler = it->second.handler;
            isCurrentCtx = true;
        }
    }

    // Stale completion for a removed/replaced source.
    if (!isCurrentCtx)
    {
        // Context was already moved to m_retiredSourceContexts
        // by removeSource or replaced by addSource. Nothing to do.
        return;
    }

    if (handler)
    {
        handler();
    }

    // Re-register the wait if source still exists with same context.
    bool rearm = false;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(srcHandle);
        rearm = (it != m_sources.end() && it->second.context == ctx);
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
        else
        {
            // Re-arm failed. Remove the dead source and fire onError.
            std::function<void()> errorCb;
            {
                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                auto it = m_sources.find(srcHandle);
                if (it != m_sources.end() && it->second.context == ctx)
                {
                    errorCb = it->second.onError;
                    it->second.context = nullptr;
                    m_sources.erase(it);
                }
            }
            if (errorCb)
            {
                errorCb();
            }
            delete ctx;
        }
    }
    else
    {
        // Source was removed while handler was running.
        // Context ownership transferred to m_retiredSourceContexts.
    }
}

void RunLoop::dispatchTimer(void *rawCtx)
{
    auto *ctx = static_cast<TimerContext *>(rawCtx);
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
        else
        {
            // Stale completion for a removed timer. Free the zombie.
            auto rit = std::find(m_retiredTimerHandles.begin(),
                                 m_retiredTimerHandles.end(), ctx);
            if (rit != m_retiredTimerHandles.end())
            {
                m_retiredTimerHandles.erase(rit);
                delete ctx;
            }
            return;
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

} // namespace vortex

#endif // defined(_WIN32)
