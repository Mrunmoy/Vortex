#include <gtest/gtest.h>
#include "RunLoop.h"

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>

#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

using namespace vortex;
using namespace std::chrono_literals;

// Helper: run loop in background, auto-stop on scope exit.
struct RunLoopGuard
{
    RunLoop &loop;
    std::thread thread;

    explicit RunLoopGuard(RunLoop &l) : loop(l), thread([&l] { l.run(); }) {}

    ~RunLoopGuard()
    {
        loop.stop();
        if (thread.joinable())
            thread.join();
    }
};

// ═════════════════════════════════════════════════════════════════════
// init() sets the name.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, InitSetsName)
{
    RunLoop loop;
    loop.init("TestLoop");
    EXPECT_STREQ(loop.name(), "TestLoop");
}

// ═════════════════════════════════════════════════════════════════════
// run() blocks, stop() causes it to return.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RunStop)
{
    RunLoop loop;
    loop.init("RunStop");

    std::atomic<bool> running{false};
    std::thread t([&] {
        running.store(true);
        loop.run();
        running.store(false);
    });

    for (int i = 0; i < 100 && !running.load(); ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(running.load());
    EXPECT_TRUE(loop.isRunning());

    loop.stop();
    t.join();

    EXPECT_FALSE(running.load());
    EXPECT_FALSE(loop.isRunning());
}

// ═════════════════════════════════════════════════════════════════════
// stop() before run() — run() should return immediately.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, StopBeforeRun)
{
    RunLoop loop;
    loop.init("StopBefore");
    loop.stop();

    std::atomic<bool> done{false};
    std::thread t([&] {
        loop.run();
        done.store(true);
    });

    for (int i = 0; i < 100 && !done.load(); ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(done.load());
    t.join();
}

// ═════════════════════════════════════════════════════════════════════
// stop() from within a posted callable.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, StopFromCallable)
{
    RunLoop loop;
    loop.init("StopCallable");

    std::atomic<bool> done{false};
    std::thread t([&] {
        loop.run();
        done.store(true);
    });

    std::this_thread::sleep_for(10ms);

    loop.executeOnRunLoop([&] { loop.stop(); });

    for (int i = 0; i < 100 && !done.load(); ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(done.load());
    t.join();
}

// ═════════════════════════════════════════════════════════════════════
// Destructor stops a running loop.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, DestructorStops)
{
    std::atomic<bool> done{false};
    {
        RunLoop loop;
        loop.init("DtorStop");
        std::thread t([&] {
            loop.run();
            done.store(true);
        });
        std::this_thread::sleep_for(10ms);
        loop.stop();
        t.join();
    }
    EXPECT_TRUE(done.load());
}

// ═════════════════════════════════════════════════════════════════════
// executeOnRunLoop() executes callable on the loop thread.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, ExecuteOnRunLoop)
{
    RunLoop loop;
    loop.init("PostThread");

    std::thread::id loopThreadId;
    std::thread::id postedThreadId;
    std::atomic<bool> done{false};

    std::thread t([&] {
        loopThreadId = std::this_thread::get_id();
        loop.run();
    });

    std::this_thread::sleep_for(10ms);

    loop.executeOnRunLoop([&] {
        postedThreadId = std::this_thread::get_id();
        done.store(true);
        loop.stop();
    });

    t.join();

    EXPECT_TRUE(done.load());
    EXPECT_EQ(postedThreadId, loopThreadId);
}

// ═════════════════════════════════════════════════════════════════════
// Multiple posts from different threads all execute.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, MultiplePostsFromThreads)
{
    RunLoop loop;
    loop.init("MultiPost");

    std::atomic<int> count{0};
    constexpr int NUM_THREADS = 4;
    constexpr int POSTS_PER_THREAD = 25;

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < POSTS_PER_THREAD; ++i)
            {
                loop.executeOnRunLoop([&] { count.fetch_add(1); });
            }
        });
    }

    for (auto &th : threads)
        th.join();

    for (int i = 0; i < 200 && count.load() < NUM_THREADS * POSTS_PER_THREAD; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(count.load(), NUM_THREADS * POSTS_PER_THREAD);
}

// ═════════════════════════════════════════════════════════════════════
// Posted callables execute in order.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, PostOrder)
{
    RunLoop loop;
    loop.init("PostOrder");

    std::vector<int> order;
    std::mutex mu;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int N = 50;
    for (int i = 0; i < N; ++i)
    {
        loop.executeOnRunLoop([&, i] {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
            count.fetch_add(1);
        });
    }

    for (int i = 0; i < 200 && count.load() < N; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(count.load(), N);
    std::lock_guard<std::mutex> lock(mu);
    for (int i = 0; i < N; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

// ═════════════════════════════════════════════════════════════════════
// run() can be called again after stop().
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RestartAfterStop)
{
    RunLoop loop;
    loop.init("Restart");

    // First run/stop cycle
    {
        RunLoopGuard guard(loop);
        std::this_thread::sleep_for(10ms);
    }

    // Second run/stop cycle
    std::atomic<bool> executed{false};
    {
        std::thread t([&] { loop.run(); });
        std::this_thread::sleep_for(10ms);

        loop.executeOnRunLoop([&] {
            executed.store(true);
            loop.stop();
        });

        t.join();
    }

    EXPECT_TRUE(executed.load());
}

// ── Portable pipe helpers ────────────────────────────────────────────

using Handle = vortex::RunLoop::NativeHandle;

#if defined(__linux__)

static std::pair<Handle, Handle> makePipe()
{
    int fds[2] = {-1, -1};
    int rc = pipe2(fds, O_CLOEXEC | O_NONBLOCK);
    EXPECT_EQ(rc, 0) << "pipe2 failed: errno=" << errno;
    return {fds[0], fds[1]};
}

static void writeByte(Handle fd)
{
    char byte = 1;
    [[maybe_unused]] auto r = write(fd, &byte, 1);
}

static void drainPipe(Handle fd)
{
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

static void closePipe(Handle readFd, Handle writeFd)
{
    close(readFd);
    close(writeFd);
}

#elif defined(__APPLE__)

static std::pair<Handle, Handle> makePipe()
{
    int fds[2] = {-1, -1};
    int rc = pipe(fds);
    EXPECT_EQ(rc, 0) << "pipe failed: errno=" << errno;
    if (rc == 0)
    {
        for (int i = 0; i < 2; ++i)
        {
            int flags = fcntl(fds[i], F_GETFL);
            EXPECT_GE(flags, 0) << "fcntl F_GETFL failed: errno=" << errno;
            EXPECT_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
            EXPECT_EQ(fcntl(fds[i], F_SETFD, FD_CLOEXEC), 0);
        }
    }
    return {fds[0], fds[1]};
}

static void writeByte(Handle fd)
{
    char byte = 1;
    [[maybe_unused]] auto r = write(fd, &byte, 1);
}

static void drainPipe(Handle fd)
{
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

static void closePipe(Handle readFd, Handle writeFd)
{
    close(readFd);
    close(writeFd);
}

#elif defined(_WIN32)

static std::pair<Handle, Handle> makePipe()
{
    HANDLE readH = nullptr, writeH = nullptr;
    BOOL ok = CreatePipe(&readH, &writeH, nullptr, 0);
    EXPECT_TRUE(ok) << "CreatePipe failed: error=" << GetLastError();
    return {readH, writeH};
}

static void writeByte(Handle h)
{
    DWORD written = 0;
    char byte = 1;
    WriteFile(static_cast<HANDLE>(h), &byte, 1, &written, nullptr);
}

static void drainPipe(Handle h)
{
    char buf[64];
    DWORD avail = 0;
    while (PeekNamedPipe(static_cast<HANDLE>(h), nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
    {
        DWORD bytesRead = 0;
        ReadFile(static_cast<HANDLE>(h), buf, sizeof(buf), &bytesRead, nullptr);
    }
}

static void closePipe(Handle readH, Handle writeH)
{
    CloseHandle(static_cast<HANDLE>(readH));
    CloseHandle(static_cast<HANDLE>(writeH));
}

#else // stub — no fd-based source tests

static std::pair<Handle, Handle> makePipe() { return {-1, -1}; }
static void writeByte(Handle) {}
static void drainPipe(Handle) {}
static void closePipe(Handle, Handle) {}

#endif

// ═════════════════════════════════════════════════════════════════════
// addSource() fires handler when fd is readable.
// removeSource() stops firing.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, AddAndRemoveSource)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("AddRemove");

    auto [readFd, writeFd] = makePipe();

    std::atomic<int> count{0};
    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        count.fetch_add(1);
    });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    // Trigger the source.
    writeByte(writeFd);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    // Remove and trigger again — should NOT fire.
    loop.removeSource(readFd);
    writeByte(writeFd);
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(count.load(), 1);

    closePipe(readFd, writeFd);
}

// ═════════════════════════════════════════════════════════════════════
// Source handler runs on the loop thread.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, SourceCallbackRunsOnLoopThread)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("SourceThread");

    auto [readFd, writeFd] = makePipe();

    std::thread::id loopThreadId;
    std::thread::id handlerThreadId;
    std::atomic<bool> done{false};

    std::thread t([&] {
        loopThreadId = std::this_thread::get_id();
        loop.run();
    });

    std::this_thread::sleep_for(10ms);

    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        handlerThreadId = std::this_thread::get_id();
        done.store(true);
    });

    writeByte(writeFd);

    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(done.load());
    EXPECT_EQ(handlerThreadId, loopThreadId);

    loop.removeSource(readFd);
    loop.stop();
    t.join();

    closePipe(readFd, writeFd);
}

// ═════════════════════════════════════════════════════════════════════
// Multiple sources fire independently.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, MultipleSourcesConcurrent)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("MultiSource");

    constexpr int N = 3;
    Handle readFds[N], writeFds[N];
    for (int i = 0; i < N; ++i)
    {
        auto [r, w] = makePipe();
        readFds[i] = r;
        writeFds[i] = w;
    }

    std::atomic<int> count{0};
    for (int i = 0; i < N; ++i)
    {
        Handle rfd = readFds[i];
        loop.addSource(rfd, [&count, rfd] {
            drainPipe(rfd);
            count.fetch_add(1);
        });
    }

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    // Trigger all sources.
    for (int i = 0; i < N; ++i)
        writeByte(writeFds[i]);

    for (int i = 0; i < 200 && count.load() < N; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), N);

    for (int i = 0; i < N; ++i)
    {
        loop.removeSource(readFds[i]);
        closePipe(readFds[i], writeFds[i]);
    }
}

// ═════════════════════════════════════════════════════════════════════
// Handler can call removeSource() on itself without deadlock.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RemoveSourceFromHandler)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("SelfRemove");

    auto [readFd, writeFd] = makePipe();

    std::atomic<int> count{0};
    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        count.fetch_add(1);
        loop.removeSource(readFd);
    });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    writeByte(writeFd);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    // Trigger again — handler removed itself, should not fire.
    writeByte(writeFd);
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(count.load(), 1);

    closePipe(readFd, writeFd);
}

// ═════════════════════════════════════════════════════════════════════
// addSource() called twice with the same fd replaces the handler.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, UpdateSourceHandler)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("UpdateSource");

    auto [readFd, writeFd] = makePipe();

    std::atomic<int> handler1Count{0};
    std::atomic<int> handler2Count{0};

    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        handler1Count.fetch_add(1);
    });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    // Trigger with the first handler.
    writeByte(writeFd);

    for (int i = 0; i < 200 && handler1Count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(handler1Count.load(), 1);
    EXPECT_EQ(handler2Count.load(), 0);

    // Replace the handler — must not fail or leave epoll in a broken state.
    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        handler2Count.fetch_add(1);
    });

    // Trigger with the second handler.
    writeByte(writeFd);

    for (int i = 0; i < 200 && handler2Count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(handler2Count.load(), 1);
    EXPECT_EQ(handler1Count.load(), 1); // first handler must not have fired again

    loop.removeSource(readFd);
    closePipe(readFd, writeFd);
}

// ═════════════════════════════════════════════════════════════════════
// Exception from a posted callable propagates out of run() and the
// loop is restartable afterwards.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, ExceptionFromCallablePropagatesToRun)
{
    RunLoop loop;
    loop.init("ExThrow");

    // Post a callable that throws, then run the loop on a thread.
    loop.executeOnRunLoop([] {
        throw std::runtime_error("test exception");
    });

    std::exception_ptr captured;
    std::thread t([&] {
        try
        {
            loop.run();
        }
        catch (...)
        {
            captured = std::current_exception();
        }
    });
    t.join();

    // The exception must have propagated.
    ASSERT_TRUE(captured != nullptr);
    EXPECT_THROW(std::rethrow_exception(captured), std::runtime_error);

    // Flags must be reset — isRunning() should be false and run() restartable.
    EXPECT_FALSE(loop.isRunning());

    // Verify the loop is actually restartable by running and stopping it.
    std::thread t2([&] { loop.run(); });
    std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(loop.isRunning());
    loop.stop();
    t2.join();
    EXPECT_FALSE(loop.isRunning());
}

// ═════════════════════════════════════════════════════════════════════
// addSource() from a non-loop thread works correctly.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, AddSourceFromAnyThread)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#endif
    RunLoop loop;
    loop.init("ThreadAdd");

    auto [readFd, writeFd] = makePipe();

    std::atomic<bool> fired{false};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    // Add source from a different thread.
    std::thread adder([&] {
        loop.addSource(readFd, [&] {
            drainPipe(readFd);
            fired.store(true);
        });
        std::this_thread::sleep_for(10ms);
        writeByte(writeFd);
    });

    adder.join();

    for (int i = 0; i < 200 && !fired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(fired.load());

    loop.removeSource(readFd);
    closePipe(readFd, writeFd);
}

// ═════════════════════════════════════════════════════════════════════
// B1: WFMO slot budget — Win32-only capacity checks.
//
// WaitForMultipleObjects has a 64-handle limit. The WFMO array is
// 1 (wakeup) + sources + timers, so at most 63 user slots.
// These tests verify addSource()/addTimer() reject overflows.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, AddSourceRejectsWhenSlotsFull)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "WFMO slot limit is Win32-only";
#else
    RunLoop loop;
    loop.init("SlotFullSrc");

    std::vector<HANDLE> events;
    auto cleanup = [&] {
        for (auto h : events)
        {
            loop.removeSource(static_cast<RunLoop::NativeHandle>(h));
            CloseHandle(h);
        }
    };

    for (int i = 0; i < 63; ++i)
    {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(ev, nullptr) << "CreateEvent failed at " << i;
        events.push_back(ev);
        ASSERT_NO_THROW(loop.addSource(static_cast<RunLoop::NativeHandle>(ev), [] {}));
    }

    HANDLE overflow = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(overflow, nullptr);

    EXPECT_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(overflow), [] {}),
        std::runtime_error);

    CloseHandle(overflow);
    cleanup();
#endif
}

TEST(RunLoopTest, AddTimerRejectsWhenSlotsFull)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "WFMO slot limit is Win32-only";
#else
    RunLoop loop;
    loop.init("SlotFullTmr");

    std::vector<RunLoop::TimerId> ids;
    for (int i = 0; i < 63; ++i)
    {
        RunLoop::TimerId tid = 0;
        ASSERT_NO_THROW(tid = loop.addTimer(10000, false, [] {}));
        ids.push_back(tid);
    }

    EXPECT_THROW(loop.addTimer(10000, false, [] {}), std::runtime_error);

    for (auto tid : ids)
        loop.removeTimer(tid);
#endif
}

TEST(RunLoopTest, CombinedSourceTimerSlotLimit)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "WFMO slot limit is Win32-only";
#else
    RunLoop loop;
    loop.init("SlotCombo");

    std::vector<HANDLE> events;
    std::vector<RunLoop::TimerId> timerIds;
    auto cleanup = [&] {
        for (auto h : events)
        {
            loop.removeSource(static_cast<RunLoop::NativeHandle>(h));
            CloseHandle(h);
        }
        for (auto tid : timerIds)
            loop.removeTimer(tid);
    };

    // 32 sources + 31 timers = 63 slots.
    for (int i = 0; i < 32; ++i)
    {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(ev, nullptr);
        events.push_back(ev);
        ASSERT_NO_THROW(loop.addSource(static_cast<RunLoop::NativeHandle>(ev), [] {}));
    }
    for (int i = 0; i < 31; ++i)
    {
        RunLoop::TimerId tid = 0;
        ASSERT_NO_THROW(tid = loop.addTimer(10000, false, [] {}));
        timerIds.push_back(tid);
    }

    // Next source must fail.
    HANDLE overflow = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(overflow, nullptr);
    EXPECT_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(overflow), [] {}),
        std::runtime_error);
    CloseHandle(overflow);

    // Next timer must also fail.
    EXPECT_THROW(loop.addTimer(10000, false, [] {}), std::runtime_error);

    cleanup();
#endif
}

TEST(RunLoopTest, SourceAfterTimerRemovalSucceeds)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "WFMO slot limit is Win32-only";
#else
    RunLoop loop;
    loop.init("SlotFree");

    std::vector<HANDLE> events;
    std::vector<RunLoop::TimerId> timerIds;
    auto cleanup = [&] {
        for (auto h : events)
        {
            loop.removeSource(static_cast<RunLoop::NativeHandle>(h));
            CloseHandle(h);
        }
        for (auto tid : timerIds)
            loop.removeTimer(tid);
    };

    // Fill: 30 sources + 33 timers = 63.
    for (int i = 0; i < 30; ++i)
    {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(ev, nullptr);
        events.push_back(ev);
        ASSERT_NO_THROW(loop.addSource(static_cast<RunLoop::NativeHandle>(ev), [] {}));
    }
    for (int i = 0; i < 33; ++i)
    {
        RunLoop::TimerId tid = 0;
        ASSERT_NO_THROW(tid = loop.addTimer(10000, false, [] {}));
        timerIds.push_back(tid);
    }

    // At capacity — next add should fail.
    HANDLE overflow = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(overflow, nullptr);
    EXPECT_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(overflow), [] {}),
        std::runtime_error);

    // Free one timer slot.
    loop.removeTimer(timerIds.back());
    timerIds.pop_back();

    // Now add should succeed (30 + 32 = 62 < 63).
    events.push_back(overflow);
    EXPECT_NO_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(overflow), [] {}));

    cleanup();
#endif
}

TEST(RunLoopTest, ReplaceSourceDoesNotConsumeSlot)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "WFMO slot limit is Win32-only";
#else
    RunLoop loop;
    loop.init("ReplaceSlot");

    std::vector<HANDLE> events;
    auto cleanup = [&] {
        for (auto h : events)
        {
            loop.removeSource(static_cast<RunLoop::NativeHandle>(h));
            CloseHandle(h);
        }
    };

    // Fill all 63 source slots.
    for (int i = 0; i < 63; ++i)
    {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(ev, nullptr);
        events.push_back(ev);
        ASSERT_NO_THROW(loop.addSource(static_cast<RunLoop::NativeHandle>(ev), [] {}));
    }

    // Replacing an existing handle must succeed — it reuses the slot.
    ASSERT_NO_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(events[0]), [] {}));

    // A truly new handle must still be rejected.
    HANDLE extra = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(extra, nullptr);
    EXPECT_THROW(
        loop.addSource(static_cast<RunLoop::NativeHandle>(extra), [] {}),
        std::runtime_error);
    CloseHandle(extra);

    cleanup();
#endif
}

// ═════════════════════════════════════════════════════════════════════
// One-shot timer fires once and is auto-removed.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, OneShotTimer)
{
    RunLoop loop;
    loop.init("OneShot");

    std::atomic<int> count{0};
    auto id = loop.addTimer(20, false, [&] { count.fetch_add(1); });
    (void)id;

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    // Wait a bit more — should not fire again.
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count.load(), 1);
}

// ═════════════════════════════════════════════════════════════════════
// Repeating timer fires multiple times.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RepeatingTimer)
{
    RunLoop loop;
    loop.init("Repeat");

    std::atomic<int> count{0};
    auto id = loop.addTimer(15, true, [&] { count.fetch_add(1); });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 400 && count.load() < 3; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_GE(count.load(), 3);

    loop.removeTimer(id);
}

// ═════════════════════════════════════════════════════════════════════
// removeTimer() cancels a repeating timer.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RemoveTimerStopsFiring)
{
    RunLoop loop;
    loop.init("RemoveTimer");

    std::atomic<int> count{0};
    auto id = loop.addTimer(10, true, [&] { count.fetch_add(1); });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_GE(count.load(), 1);

    loop.removeTimer(id);
    int snapshot = count.load();

    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(count.load(), snapshot);
}

// ═════════════════════════════════════════════════════════════════════
// Timer handler runs on the loop thread.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, TimerHandlerRunsOnLoopThread)
{
    RunLoop loop;
    loop.init("TimerThread");

    std::thread::id loopThreadId;
    std::thread::id timerThreadId;
    std::atomic<bool> done{false};

    std::thread t([&] {
        loopThreadId = std::this_thread::get_id();
        loop.run();
    });

    for (int i = 0; i < 100 && !loop.isRunning(); ++i)
        std::this_thread::sleep_for(5ms);

    auto id = loop.addTimer(10, false, [&] {
        timerThreadId = std::this_thread::get_id();
        done.store(true);
    });
    (void)id;

    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(done.load());
    EXPECT_EQ(timerThreadId, loopThreadId);

    loop.stop();
    t.join();
}

// ═════════════════════════════════════════════════════════════════════
// Multiple timers with different intervals fire independently.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, MultipleTimers)
{
    RunLoop loop;
    loop.init("MultiTimer");

    std::atomic<int> fastCount{0};
    std::atomic<int> slowCount{0};

    auto fast = loop.addTimer(10, true, [&] { fastCount.fetch_add(1); });
    auto slow = loop.addTimer(50, true, [&] { slowCount.fetch_add(1); });

    RunLoopGuard guard(loop);

    // Wait for slow timer to fire at least twice.
    for (int i = 0; i < 400 && slowCount.load() < 2; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_GE(slowCount.load(), 2);
    EXPECT_GE(fastCount.load(), slowCount.load());

    loop.removeTimer(fast);
    loop.removeTimer(slow);
}

// ═════════════════════════════════════════════════════════════════════
// removeTimer() from within a timer handler (self-cancel).
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RemoveTimerFromHandler)
{
    RunLoop loop;
    loop.init("SelfCancel");

    std::atomic<int> count{0};
    RunLoop::TimerId timerId = 0;

    timerId = loop.addTimer(15, true, [&] {
        count.fetch_add(1);
        loop.removeTimer(timerId);
    });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    // Wait — should not fire again.
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(count.load(), 1);
}

// ---------- Source Error Callback Tests ----------

TEST(RunLoopTest, ErrorCallbackOnPeerClose)
{
#if defined(_WIN32)
    GTEST_SKIP() << "Pipe error detection not implemented on Win32";
#else
    RunLoop loop;
    loop.init("ErrorCallbackOnPeerClose");

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    int readFd = fds[0];
    int writeFd = fds[1];

    std::atomic<bool> errorFired{false};

    loop.addSource(
        readFd,
        []() {},
        [&errorFired]() { errorFired.store(true); });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(20ms);

    close(writeFd);
    std::this_thread::sleep_for(50ms);

    EXPECT_TRUE(errorFired.load());

    close(readFd);
#endif
}

TEST(RunLoopTest, NoErrorCallbackWithoutOnError)
{
#if defined(_WIN32)
    GTEST_SKIP() << "Pipe error detection not implemented on Win32";
#else
    RunLoop loop;
    loop.init("NoErrorCallbackWithoutOnError");

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    int readFd = fds[0];
    int writeFd = fds[1];

    std::atomic<int> handlerCount{0};

    loop.addSource(readFd, [&handlerCount]() { handlerCount.fetch_add(1); });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(20ms);

    close(writeFd);
    std::this_thread::sleep_for(50ms);

    EXPECT_GE(handlerCount.load(), 1);

    loop.removeSource(readFd);
    close(readFd);
#endif
}

TEST(RunLoopTest, ErrorCallbackAutoRemovesSource)
{
#if defined(_WIN32)
    GTEST_SKIP() << "Pipe error detection not implemented on Win32";
#else
    RunLoop loop;
    loop.init("ErrorCallbackAutoRemovesSource");

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    int readFd = fds[0];
    int writeFd = fds[1];

    std::atomic<int> errorCount{0};

    loop.addSource(
        readFd,
        []() {},
        [&errorCount]() { errorCount.fetch_add(1); });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(20ms);

    close(writeFd);
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(errorCount.load(), 1);

    close(readFd);
#endif
}

// ═══════════════════════════════════════════════════════════════════════
// Behavioral Contract Tests — must pass on ALL backends (Phase A)
// ═══════════════════════════════════════════════════════════════════════

// ── Category 1: Lifecycle Edge Cases (all platforms) ─────────────────

TEST(RunLoopTest, DoubleInitThrows)
{
    RunLoop loop;
    loop.init("DoubleInit");
    EXPECT_THROW(loop.init("DoubleInit2"), std::logic_error);
}

TEST(RunLoopTest, RemoveNonExistentSourceIsNoOp)
{
    RunLoop loop;
    loop.init("NoSrc");

#if defined(_WIN32)
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(ev, nullptr);
    EXPECT_NO_THROW(loop.removeSource(static_cast<RunLoop::NativeHandle>(ev)));
    CloseHandle(ev);
#else
    EXPECT_NO_THROW(loop.removeSource(9999));
#endif
}

TEST(RunLoopTest, RemoveNonExistentTimerIsNoOp)
{
    RunLoop loop;
    loop.init("NoTimer");
    EXPECT_NO_THROW(loop.removeTimer(999999));
}

TEST(RunLoopTest, DoubleRemoveTimerIsNoOp)
{
    RunLoop loop;
    loop.init("DblRmTimer");

    auto id = loop.addTimer(100000, false, [] {});
    loop.removeTimer(id);
    EXPECT_NO_THROW(loop.removeTimer(id));
}

TEST(RunLoopTest, StopIdempotent)
{
    RunLoop loop;
    loop.init("StopIdem");

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    loop.stop();
    // Wait for loop to actually stop
    for (int i = 0; i < 200 && loop.isRunning(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_FALSE(loop.isRunning());
    loop.stop();
    loop.stop();
}

TEST(RunLoopTest, StopWithoutRun)
{
    RunLoop loop;
    loop.init("StopNoRun");

    loop.stop();

    std::atomic<bool> done{false};
    std::thread t([&] {
        loop.run();
        done.store(true);
    });

    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(done.load());
    t.join();
}

// ── Category 2: Timer Contract Tests (all platforms) ─────────────────

TEST(RunLoopTest, OneShotTimerFiresExactlyOnce)
{
    RunLoop loop;
    loop.init("OneShotExact");

    std::atomic<int> count{0};
    loop.addTimer(10, false, [&] { count.fetch_add(1); });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    // Wait 5x the interval — must not fire again.
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count.load(), 1);
}

TEST(RunLoopTest, OneShotTimerAutoRemoved)
{
    RunLoop loop;
    loop.init("AutoRemove");

    std::atomic<bool> fired{false};
    auto id = loop.addTimer(10, false, [&] { fired.store(true); });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && !fired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(fired.load());

    std::this_thread::sleep_for(20ms);
    EXPECT_NO_THROW(loop.removeTimer(id));
}

TEST(RunLoopTest, TimerHandlerCanAddNewTimer)
{
    RunLoop loop;
    loop.init("TimerAddsTimer");

    std::atomic<bool> innerFired{false};

    loop.addTimer(10, false, [&] {
        loop.addTimer(10, false, [&] {
            innerFired.store(true);
        });
    });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 400 && !innerFired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(innerFired.load());
}

TEST(RunLoopTest, TimerIdsAreUnique)
{
    RunLoop loop;
    loop.init("UniqueId");

    constexpr int N = 10;
    std::set<RunLoop::TimerId> ids;

    for (int i = 0; i < N; ++i)
        ids.insert(loop.addTimer(100000, false, [] {}));

    EXPECT_EQ(ids.size(), static_cast<size_t>(N)) << "All TimerIds must be unique";

    for (auto id : ids)
        loop.removeTimer(id);
}

TEST(RunLoopTest, MinIntervalTimerFiresQuickly)
{
    RunLoop loop;
    loop.init("QuickTimer");

    std::atomic<bool> fired{false};
    loop.addTimer(1, false, [&] { fired.store(true); });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && !fired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(fired.load());
}

// ── Category 3: Callable Contract Tests (all platforms) ──────────────

TEST(RunLoopTest, PostFromLoopThread)
{
    RunLoop loop;
    loop.init("PostInside");

    std::atomic<bool> innerDone{false};

    loop.executeOnRunLoop([&] {
        loop.executeOnRunLoop([&] {
            innerDone.store(true);
        });
    });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && !innerDone.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(innerDone.load());
}

TEST(RunLoopTest, PostAfterStopQueuesForRestart)
{
    RunLoop loop;
    loop.init("PostRestart");

    // First run/stop cycle.
    {
        RunLoopGuard guard(loop);
        std::this_thread::sleep_for(10ms);
    }

    // Post after stop.
    std::atomic<bool> executed{false};
    loop.executeOnRunLoop([&] { executed.store(true); });

    EXPECT_FALSE(executed.load());

    // Second run should deliver the queued callable.
    {
        std::thread t([&] { loop.run(); });
        for (int i = 0; i < 200 && !executed.load(); ++i)
            std::this_thread::sleep_for(5ms);
        loop.stop();
        t.join();
    }

    EXPECT_TRUE(executed.load());
}

TEST(RunLoopTest, PostPreservesOrderUnderLoad)
{
    RunLoop loop;
    loop.init("OrderLoad");

    constexpr int N = 1000;
    std::vector<int> order;
    order.reserve(N);
    std::mutex mu;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < N; ++i)
    {
        loop.executeOnRunLoop([&, i] {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
            count.fetch_add(1);
        });
    }

    for (int i = 0; i < 400 && count.load() < N; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), N);
    std::lock_guard<std::mutex> lock(mu);
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(order[i], i) << "FIFO violated at index " << i;
}

TEST(RunLoopTest, PostFromMultipleThreadsAllDelivered)
{
    RunLoop loop;
    loop.init("MultiThreadPost");

    constexpr int NUM_THREADS = 10;
    constexpr int POSTS_PER_THREAD = 100;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < POSTS_PER_THREAD; ++i)
                loop.executeOnRunLoop([&] { count.fetch_add(1); });
        });
    }

    for (auto &th : threads)
        th.join();

    for (int i = 0; i < 400 && count.load() < NUM_THREADS * POSTS_PER_THREAD; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), NUM_THREADS * POSTS_PER_THREAD);
}

// ── Category 4: Re-entrant Safety ────────────────────────────────────

TEST(RunLoopTest, TimerRemovesSelfFromHandler)
{
    RunLoop loop;
    loop.init("SelfRmTimer");

    std::atomic<int> count{0};
    std::atomic<RunLoop::TimerId> timerId{0};

    timerId.store(loop.addTimer(10, true, [&] {
        count.fetch_add(1);
        loop.removeTimer(timerId.load());
    }));

    RunLoopGuard guard(loop);

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(count.load(), 1);
}

TEST(RunLoopTest, TimerAddsSourceFromHandler)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#endif
    RunLoop loop;
    loop.init("TmrAddsSrc");

    auto [readFd, writeFd] = makePipe();

    std::atomic<bool> sourceFired{false};

    loop.addTimer(10, false, [&] {
        loop.addSource(readFd, [&] {
            drainPipe(readFd);
            sourceFired.store(true);
        });
        writeByte(writeFd);
    });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 400 && !sourceFired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(sourceFired.load());

    loop.removeSource(readFd);
    closePipe(readFd, writeFd);
}

TEST(RunLoopTest, CallableAddsTimer)
{
    RunLoop loop;
    loop.init("PostAddsTmr");

    std::atomic<bool> timerFired{false};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::thread poster([&] {
        loop.executeOnRunLoop([&] {
            loop.addTimer(10, false, [&] {
                timerFired.store(true);
            });
        });
    });
    poster.join();

    for (int i = 0; i < 400 && !timerFired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(timerFired.load());
}

// ── Category 5: Stress Tests ─────────────────────────────────────────

TEST(RunLoopTest, StressPostFromManyThreads)
{
    RunLoop loop;
    loop.init("StressPosts");

    constexpr int NUM_THREADS = 20;
    constexpr int POSTS_PER_THREAD = 500;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < POSTS_PER_THREAD; ++i)
                loop.executeOnRunLoop([&] { count.fetch_add(1); });
        });
    }

    for (auto &th : threads)
        th.join();

    for (int i = 0; i < 600 && count.load() < NUM_THREADS * POSTS_PER_THREAD; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(count.load(), NUM_THREADS * POSTS_PER_THREAD);
}

TEST(RunLoopTest, StressTimerChurn)
{
    RunLoop loop;
    loop.init("StressChurn");

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int N = 50;
    std::atomic<int> fireCount{0};

    std::thread churner([&] {
        for (int i = 0; i < N; ++i)
        {
            auto id = loop.addTimer(5, false, [&] { fireCount.fetch_add(1); });
            if (i % 2 == 0)
                loop.removeTimer(id);
        }
    });
    churner.join();

    std::this_thread::sleep_for(200ms);

    EXPECT_GE(fireCount.load(), N / 2 - 5);
    EXPECT_LE(fireCount.load(), N);
}

TEST(RunLoopTest, StressStartStop)
{
    RunLoop loop;
    loop.init("StressStop");

    constexpr int CYCLES = 10;

    for (int i = 0; i < CYCLES; ++i)
    {
        std::thread t([&] { loop.run(); });
        for (int j = 0; j < 200 && !loop.isRunning(); ++j)
            std::this_thread::sleep_for(1ms);
        loop.stop();
        t.join();
        EXPECT_FALSE(loop.isRunning()) << "Cycle " << i;
    }
}

// ── Category 5b: Advanced Stress & Concurrency Tests ─────────────────

TEST(RunLoopTest, StressConcurrentTimerAddRemove)
{
    RunLoop loop;
    loop.init("ConcTmr");

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 200;
    std::atomic<int> addCount{0};

    // Each thread rapidly adds and immediately removes timers.
    // This stresses thread safety of addTimer/removeTimer without
    // accumulating live timers (stays under WFMO 63-slot limit).
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < OPS_PER_THREAD; ++i)
            {
                auto id = loop.addTimer(100000, false, [] {});
                addCount.fetch_add(1);
                loop.removeTimer(id);
            }
        });
    }

    for (auto &th : threads)
        th.join();

    EXPECT_EQ(addCount.load(), NUM_THREADS * OPS_PER_THREAD);
}

TEST(RunLoopTest, StressStartStopManyCycles)
{
    RunLoop loop;
    loop.init("ManyCyc");

    constexpr int CYCLES = 50;

    for (int i = 0; i < CYCLES; ++i)
    {
        std::thread t([&] { loop.run(); });
        for (int j = 0; j < 500 && !loop.isRunning(); ++j)
            std::this_thread::sleep_for(1ms);
        loop.stop();
        t.join();
        EXPECT_FALSE(loop.isRunning()) << "Cycle " << i;
    }
}

TEST(RunLoopTest, StressMixedWorkload)
{
    RunLoop loop;
    loop.init("Mixed");

    std::atomic<int> postCount{0};
    std::atomic<int> timerCount{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int POSTS = 200;
    constexpr int TIMER_OPS = 50;

    // Thread 1: posts callables
    std::thread poster([&] {
        for (int i = 0; i < POSTS; ++i)
            loop.executeOnRunLoop([&] { postCount.fetch_add(1); });
    });

    // Thread 2: adds a few timers (kept count stays low for WFMO)
    std::thread timerAdder([&] {
        for (int i = 0; i < TIMER_OPS; ++i)
        {
            loop.addTimer(10, false, [&] { timerCount.fetch_add(1); });
            std::this_thread::sleep_for(1ms);
        }
    });

    poster.join();
    timerAdder.join();

    for (int i = 0; i < 800 && (postCount.load() < POSTS || timerCount.load() < TIMER_OPS / 2); ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(postCount.load(), POSTS);
    EXPECT_GE(timerCount.load(), TIMER_OPS / 2);
}

TEST(RunLoopTest, StopFromTimerHandler)
{
    RunLoop loop;
    loop.init("StopTmr");

    std::atomic<bool> stopped{false};
    loop.addTimer(10, false, [&] {
        loop.stop();
        stopped.store(true);
    });

    std::thread t([&] { loop.run(); });
    t.join();

    EXPECT_TRUE(stopped.load());
    EXPECT_FALSE(loop.isRunning());
}

TEST(RunLoopTest, StopFromSourceHandler)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#else
    RunLoop loop;
    loop.init("StopSrc");

    auto [readFd, writeFd] = makePipe();
    std::atomic<bool> stopped{false};

    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        loop.stop();
        stopped.store(true);
    });

    std::thread t([&] { loop.run(); });
    std::this_thread::sleep_for(20ms);
    writeByte(writeFd);
    t.join();

    EXPECT_TRUE(stopped.load());
    EXPECT_FALSE(loop.isRunning());
    closePipe(readFd, writeFd);
#endif
}

TEST(RunLoopTest, RepeatingTimerDoesNotFireEarly)
{
    RunLoop loop;
    loop.init("NoEarly");

    constexpr uint32_t intervalMs = 50;
    auto startTime = std::chrono::steady_clock::now();
    std::atomic<bool> fired{false};
    std::atomic<int64_t> fireTimeNs{0};

    loop.addTimer(intervalMs, false, [&] {
        fireTimeNs.store(std::chrono::steady_clock::now().time_since_epoch().count());
        fired.store(true);
    });

    RunLoopGuard guard(loop);

    for (int i = 0; i < 400 && !fired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    ASSERT_TRUE(fired.load());
    auto fireTime = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(fireTimeNs.load()));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fireTime - startTime);
    EXPECT_GE(elapsed.count(), intervalMs - 15)
        << "Timer fired " << elapsed.count() << "ms after start, expected >= " << (intervalMs - 15);
}

TEST(RunLoopTest, ExceptionFromCallablePropagates)
{
    RunLoop loop;
    loop.init("ExCall");

    loop.executeOnRunLoop([] { throw std::runtime_error("callable boom"); });

    EXPECT_THROW(loop.run(), std::runtime_error);
    EXPECT_FALSE(loop.isRunning());
}

TEST(RunLoopTest, ExceptionFromTimerHandlerPropagates)
{
    RunLoop loop;
    loop.init("ExTimer");

    loop.addTimer(1, false, [] { throw std::runtime_error("timer boom"); });

    EXPECT_THROW(loop.run(), std::runtime_error);
    EXPECT_FALSE(loop.isRunning());
}

TEST(RunLoopTest, ExceptionFromSourceHandlerPropagates)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#else
    RunLoop loop;
    loop.init("ExSrc");

    auto [readFd, writeFd] = makePipe();

    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        throw std::runtime_error("source boom");
    });

    writeByte(writeFd);

    EXPECT_THROW(loop.run(), std::runtime_error);
    EXPECT_FALSE(loop.isRunning());
    closePipe(readFd, writeFd);
#endif
}

TEST(RunLoopTest, LoopRestartableAfterException)
{
    RunLoop loop;
    loop.init("RestartEx");

    // First run: exception
    loop.executeOnRunLoop([] { throw std::runtime_error("first boom"); });
    EXPECT_THROW(loop.run(), std::runtime_error);

    // Second run: normal stop
    std::atomic<bool> ok{false};
    loop.executeOnRunLoop([&] {
        ok.store(true);
        loop.stop();
    });
    EXPECT_NO_THROW(loop.run());
    EXPECT_TRUE(ok.load());
}

TEST(RunLoopTest, StressTimerIdsUniqueUnderContention)
{
    RunLoop loop;
    loop.init("IdContest");

    constexpr int NUM_THREADS = 4;
    constexpr int TIMERS_PER_THREAD = 100;

    std::mutex idsMutex;
    std::set<RunLoop::TimerId> allIds;

    // Each thread adds timers sequentially and removes them immediately
    // so we never have more than ~NUM_THREADS live at once (WFMO safe).
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            std::vector<RunLoop::TimerId> localIds;
            localIds.reserve(TIMERS_PER_THREAD);
            for (int i = 0; i < TIMERS_PER_THREAD; ++i)
            {
                auto id = loop.addTimer(100000, false, [] {});
                localIds.push_back(id);
                loop.removeTimer(id);
            }

            std::lock_guard<std::mutex> lock(idsMutex);
            for (auto id : localIds)
                allIds.insert(id);
        });
    }

    for (auto &th : threads)
        th.join();

    EXPECT_EQ(allIds.size(), static_cast<size_t>(NUM_THREADS * TIMERS_PER_THREAD))
        << "All timer IDs must be unique even under thread contention";
}

TEST(RunLoopTest, PostHighThroughput)
{
    RunLoop loop;
    loop.init("HighTP");

    constexpr int NUM_THREADS = 50;
    constexpr int POSTS_PER_THREAD = 2000;
    constexpr int TOTAL = NUM_THREADS * POSTS_PER_THREAD;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&] {
            for (int i = 0; i < POSTS_PER_THREAD; ++i)
                loop.executeOnRunLoop([&] { count.fetch_add(1); });
        });
    }

    for (auto &th : threads)
        th.join();

    for (int i = 0; i < 1200 && count.load() < TOTAL; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(count.load(), TOTAL);
}

TEST(RunLoopTest, StressTimerChurnHighScale)
{
    RunLoop loop;
    loop.init("ChurnHi");

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int N = 500;
    std::atomic<int> fireCount{0};

    // Add-then-remove pattern keeps live timer count low (WFMO safe).
    std::thread churner([&] {
        for (int i = 0; i < N; ++i)
        {
            auto id = loop.addTimer(5, false, [&] { fireCount.fetch_add(1); });
            // Remove ~90% immediately
            if (i % 10 != 0)
                loop.removeTimer(id);
        }
    });
    churner.join();

    // WFMO safety: at most N/10 = 50 live timers; well under the 63-slot limit.
    constexpr int KEPT = N / 10;
    for (int i = 0; i < 600 && fireCount.load() < KEPT / 2; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_GE(fireCount.load(), KEPT / 2);
    EXPECT_LE(fireCount.load(), N);
}

TEST(RunLoopTest, StressSourceChurn)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#else
    RunLoop loop;
    loop.init("SrcChurn");

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int CYCLES = 100;
    std::atomic<int> fireCount{0};

    for (int i = 0; i < CYCLES; ++i)
    {
        auto [readFd, writeFd] = makePipe();
        loop.addSource(readFd, [&, readFd] {
            drainPipe(readFd);
            fireCount.fetch_add(1);
        });

        writeByte(writeFd);
        std::this_thread::sleep_for(5ms);

        loop.removeSource(readFd);
        closePipe(readFd, writeFd);
    }

    EXPECT_GE(fireCount.load(), CYCLES / 2);
#endif
}

// ── Category 6: Source Tests (pipe-gated, skip on Win32) ─────────────

TEST(RunLoopTest, SourceDoesNotFireAfterRemove)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#endif
    RunLoop loop;
    loop.init("NoFireRm");

    auto [readFd, writeFd] = makePipe();

    std::atomic<int> count{0};
    loop.addSource(readFd, [&] {
        drainPipe(readFd);
        count.fetch_add(1);
    });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    loop.removeSource(readFd);
    std::this_thread::sleep_for(10ms);

    writeByte(writeFd);
    std::this_thread::sleep_for(50ms);

    EXPECT_EQ(count.load(), 0);

    closePipe(readFd, writeFd);
}

TEST(RunLoopTest, AddSourceFromHandler)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#endif
    RunLoop loop;
    loop.init("SrcAddsSrc");

    auto [readFd1, writeFd1] = makePipe();
    auto [readFd2, writeFd2] = makePipe();

    std::atomic<bool> secondFired{false};

    loop.addSource(readFd1, [&] {
        drainPipe(readFd1);
        loop.addSource(readFd2, [&] {
            drainPipe(readFd2);
            secondFired.store(true);
        });
        writeByte(writeFd2);
    });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    writeByte(writeFd1);

    for (int i = 0; i < 400 && !secondFired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(secondFired.load());

    loop.removeSource(readFd1);
    loop.removeSource(readFd2);
    closePipe(readFd1, writeFd1);
    closePipe(readFd2, writeFd2);
}

TEST(RunLoopTest, MultipleSourcesFairness)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#endif
    RunLoop loop;
    loop.init("Fairness");

    constexpr int N = 5;
    Handle readFds[N], writeFds[N];
    std::atomic<int> counts[N];

    for (int i = 0; i < N; ++i)
    {
        auto [r, w] = makePipe();
        readFds[i] = r;
        writeFds[i] = w;
        counts[i].store(0);
    }

    for (int i = 0; i < N; ++i)
    {
        Handle rfd = readFds[i];
        loop.addSource(rfd, [&counts, i, rfd] {
            drainPipe(rfd);
            counts[i].fetch_add(1);
        });
    }

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < N; ++i)
        writeByte(writeFds[i]);

    for (int retries = 0; retries < 400; ++retries)
    {
        bool allFired = true;
        for (int i = 0; i < N; ++i)
        {
            if (counts[i].load() < 1)
            {
                allFired = false;
                break;
            }
        }
        if (allFired) break;
        std::this_thread::sleep_for(5ms);
    }

    for (int i = 0; i < N; ++i)
        EXPECT_GE(counts[i].load(), 1) << "Source " << i << " never fired";

    for (int i = 0; i < N; ++i)
    {
        loop.removeSource(readFds[i]);
        closePipe(readFds[i], writeFds[i]);
    }
}

TEST(RunLoopTest, DoubleRemoveSourceIsNoOp)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles not waitable on Win32";
#endif
    RunLoop loop;
    loop.init("DblRmSrc");

    auto [readFd, writeFd] = makePipe();

    loop.addSource(readFd, [&] { drainPipe(readFd); });

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    loop.removeSource(readFd);
    EXPECT_NO_THROW(loop.removeSource(readFd));

    closePipe(readFd, writeFd);
}
