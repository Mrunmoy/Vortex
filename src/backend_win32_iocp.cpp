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
// Context lifetime — unified zombie pattern:
//   Both SourceContext and TimerContext follow a two-phase retirement:
//
//   1. removeSource/removeTimer: Cancel the OS resource (non-blocking),
//      move context to the retired list as a "zombie".
//   2. Phase 1 (run loop top): Synchronize with OS callbacks
//      (UnregisterWaitEx/WaitForThreadpoolTimerCallbacks) and mark the
//      context as "synchronized" (waitHandle=nullptr / tpTimer=nullptr).
//      Contexts that were ALREADY synchronized on a prior iteration
//      are deleted (they've had ≥1 GQCS cycle to drain stale completions).
//   3. Phase 4 (dispatch): If a stale completion arrives for a zombie,
//      delete the context immediately (fast path).
//
//   This guarantees that no context is freed while a stale IOCP
//   completion or threadpool callback can still reference it.

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

    // Synchronously unregister source waits and free all contexts.
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

        for (void *p : m_retiredSourceContexts)
        {
            auto *ctx = static_cast<SourceContext *>(p);
            if (ctx->waitHandle)
            {
                UnregisterWaitEx(ctx->waitHandle, INVALID_HANDLE_VALUE);
            }
            delete ctx;
        }
        m_retiredSourceContexts.clear();
    }

    // Cancel, close, and free all timer contexts.
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
            // ── Phase 1: Retire zombie contexts ─────────────────────
            // Two-pass: synchronize OS resources, then free contexts
            // that were already synchronized on a PRIOR iteration
            // (giving stale completions ≥1 GQCS cycle to drain).
            {
                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                auto sit = m_retiredSourceContexts.begin();
                while (sit != m_retiredSourceContexts.end())
                {
                    auto *ctx = static_cast<SourceContext *>(*sit);
                    if (ctx->waitHandle != nullptr)
                    {
                        // First visit: block until TP callback completes.
                        UnregisterWaitEx(ctx->waitHandle, INVALID_HANDLE_VALUE);
                        ctx->waitHandle = nullptr;
                        ++sit;
                    }
                    else
                    {
                        // Already synchronized on a prior iteration.
                        delete ctx;
                        sit = m_retiredSourceContexts.erase(sit);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_timersMutex);
                auto tit = m_retiredTimerHandles.begin();
                while (tit != m_retiredTimerHandles.end())
                {
                    auto *ctx = static_cast<TimerContext *>(*tit);
                    if (ctx->tpTimer != nullptr)
                    {
                        // First visit: close TP resource.
                        closeTimerResource(ctx);
                        ++tit;
                    }
                    else
                    {
                        // Already closed on a prior iteration.
                        delete ctx;
                        tit = m_retiredTimerHandles.erase(tit);
                    }
                }
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

    // Hold lock across map insert AND RegisterWaitForSingleObject to
    // prevent a concurrent removeSource from seeing the entry before
    // the wait is registered (Findings 3).
    std::lock_guard<std::mutex> lock(m_sourcesMutex);

    auto it = m_sources.find(handle);
    if (it != m_sources.end())
    {
        it->second.handler = std::move(handler);
        it->second.onError = std::move(onError);
        delete ctx;
        return;
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
        delete ctx;
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "RunLoop::addSource: RegisterWaitForSingleObject failed");
    }
    ctx->waitHandle = waitHandle;
    m_sources[handle] = {std::move(handler), std::move(onError), ctx};

    // wakeup() after lock release (RAII destructor).
    // Note: wakeup() is safe to call with m_sourcesMutex held since
    // it only touches m_pollHandle (IOCP), not any mutex.
    wakeup();
}

// ── removeSource ────────────────────────────────────────────────────

void RunLoop::removeSource(NativeHandle handle)
{
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(handle);
        if (it == m_sources.end())
            return;

        void *rawCtx = it->second.context;
        m_sources.erase(it);

        if (rawCtx != nullptr)
        {
            // Defer to retired list. Phase 1 will synchronize with the
            // TP callback (blocking UnregisterWaitEx), and Phase 4 or
            // Phase 1 (next iteration) will free the allocation.
            m_retiredSourceContexts.push_back(rawCtx);
        }
    }
    wakeup();
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
        // Cancel future firings. Phase 1 will close the TP resource;
        // Phase 4 or Phase 1 (next iteration) will free the allocation.
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

    // Look up handler and verify context identity under lock.
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

    // Stale completion for a removed/replaced source — free the zombie.
    if (!isCurrentCtx)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto rit = std::find(m_retiredSourceContexts.begin(),
                             m_retiredSourceContexts.end(), rawCtx);
        if (rit != m_retiredSourceContexts.end())
        {
            m_retiredSourceContexts.erase(rit);
            delete ctx;
        }
        return;
    }

    if (handler)
    {
        handler();
    }

    // Re-register the wait under lock to prevent TOCTOU race with
    // removeSource (Finding 2).
    std::function<void()> errorCb;
    bool rearmFailed = false;
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(srcHandle);
        if (it != m_sources.end() && it->second.context == ctx)
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
                errorCb = it->second.onError;
                it->second.context = nullptr;
                m_sources.erase(it);
                rearmFailed = true;
            }
        }
        // else: source removed during handler — ownership in retired list
    }

    // Fire onError outside lock to prevent deadlock.
    if (rearmFailed)
    {
        if (errorCb)
        {
            errorCb();
        }
        delete ctx;
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
            // Stale completion for a removed timer — free the zombie.
            auto rit = std::find(m_retiredTimerHandles.begin(),
                                 m_retiredTimerHandles.end(), rawCtx);
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
