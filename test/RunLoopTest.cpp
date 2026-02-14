#include <gtest/gtest.h>
#include "RunLoop.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace ms;
using namespace std::chrono_literals;

// Helper: run loop in background, auto-stop on scope exit.
struct RunLoopGuard {
    RunLoop& loop;
    std::thread thread;

    explicit RunLoopGuard(RunLoop& l)
        : loop(l), thread([&l] { l.run(); }) {}

    ~RunLoopGuard() {
        loop.stop();
        if (thread.joinable()) thread.join();
    }
};

// ═════════════════════════════════════════════════════════════════════
// init() sets the name.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, InitSetsName) {
    RunLoop loop;
    loop.init("TestLoop");
    EXPECT_STREQ(loop.name(), "TestLoop");
}

// ═════════════════════════════════════════════════════════════════════
// run() blocks, stop() causes it to return.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RunStop) {
    RunLoop loop;
    loop.init("RunStop");

    std::atomic<bool> running{false};
    std::thread t([&] {
        running.store(true);
        loop.run();
        running.store(false);
    });

    for (int i = 0; i < 100 && !running.load(); ++i) {
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

TEST(RunLoopTest, StopBeforeRun) {
    RunLoop loop;
    loop.init("StopBefore");
    loop.stop();

    std::atomic<bool> done{false};
    std::thread t([&] {
        loop.run();
        done.store(true);
    });

    for (int i = 0; i < 100 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(done.load());
    t.join();
}

// ═════════════════════════════════════════════════════════════════════
// stop() from within a posted callable.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, StopFromCallable) {
    RunLoop loop;
    loop.init("StopCallable");

    std::atomic<bool> done{false};
    std::thread t([&] {
        loop.run();
        done.store(true);
    });

    std::this_thread::sleep_for(10ms);

    loop.executeOnRunLoop([&] {
        loop.stop();
    });

    for (int i = 0; i < 100 && !done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(done.load());
    t.join();
}

// ═════════════════════════════════════════════════════════════════════
// Destructor stops a running loop.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, DestructorStops) {
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

TEST(RunLoopTest, ExecuteOnRunLoop) {
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

TEST(RunLoopTest, MultiplePostsFromThreads) {
    RunLoop loop;
    loop.init("MultiPost");

    std::atomic<int> count{0};
    constexpr int NUM_THREADS = 4;
    constexpr int POSTS_PER_THREAD = 25;

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < POSTS_PER_THREAD; ++i) {
                loop.executeOnRunLoop([&] {
                    count.fetch_add(1);
                });
            }
        });
    }

    for (auto& th : threads) th.join();

    for (int i = 0; i < 200 && count.load() < NUM_THREADS * POSTS_PER_THREAD; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(count.load(), NUM_THREADS * POSTS_PER_THREAD);
}

// ═════════════════════════════════════════════════════════════════════
// Posted callables execute in order.
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, PostOrder) {
    RunLoop loop;
    loop.init("PostOrder");

    std::vector<int> order;
    std::mutex mu;
    std::atomic<int> count{0};

    RunLoopGuard guard(loop);
    std::this_thread::sleep_for(10ms);

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        loop.executeOnRunLoop([&, i] {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
            count.fetch_add(1);
        });
    }

    for (int i = 0; i < 200 && count.load() < N; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(count.load(), N);
    std::lock_guard<std::mutex> lock(mu);
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(order[i], i);
    }
}

// ═════════════════════════════════════════════════════════════════════
// run() can be called again after stop().
// ═════════════════════════════════════════════════════════════════════

TEST(RunLoopTest, RestartAfterStop) {
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
