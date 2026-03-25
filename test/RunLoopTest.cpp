#include <gtest/gtest.h>
#include "RunLoop.h"

#include <atomic>
#include <chrono>
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
    EXPECT_GT(fastCount.load(), slowCount.load());

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
