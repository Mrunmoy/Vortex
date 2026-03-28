#include <gtest/gtest.h>

#include "vortex.h"

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace
{

    struct VortexGuard
    {
        vortex_t loop = nullptr;

        VortexGuard()
        {
            EXPECT_EQ(vortex_create(&loop), VORTEX_SUCCESS);
        }

        ~VortexGuard()
        {
            vortex_destroy(loop);
        }
    };

#if defined(__linux__)
    using Handle = int;

    static std::pair<Handle, Handle> makePipe()
    {
        int fds[2] = {-1, -1};
        int rc = pipe2(fds, O_CLOEXEC | O_NONBLOCK);
        EXPECT_EQ(rc, 0);
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
    using Handle = int;

    static std::pair<Handle, Handle> makePipe()
    {
        int fds[2] = {-1, -1};
        int rc = pipe(fds);
        EXPECT_EQ(rc, 0);
        if (rc == 0)
        {
            for (int i = 0; i < 2; ++i)
            {
                int flags = fcntl(fds[i], F_GETFL);
                EXPECT_GE(flags, 0);
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
#else
    using Handle = int;

    static std::pair<Handle, Handle> makePipe() { return {-1, -1}; }
    static void writeByte(Handle) {}
    static void drainPipe(Handle) {}
    static void closePipe(Handle, Handle) {}
#endif

    static void stopLoop(void *userData)
    {
        vortex_stop(static_cast<vortex_t>(userData));
    }

} // namespace

TEST(CApiTest, CreateDestroy)
{
    vortex_t loop = nullptr;
    EXPECT_EQ(vortex_create(&loop), VORTEX_SUCCESS);
    ASSERT_NE(loop, nullptr);
    vortex_destroy(loop);
}

TEST(CApiTest, InitSetsName)
{
    VortexGuard guard;
    EXPECT_EQ(vortex_init(guard.loop, "CApi"), VORTEX_SUCCESS);
    ASSERT_NE(vortex_name(guard.loop), nullptr);
    EXPECT_STREQ(vortex_name(guard.loop), "CApi");
}

TEST(CApiTest, DoubleInitFails)
{
    VortexGuard guard;
    EXPECT_EQ(vortex_init(guard.loop, "First"), VORTEX_SUCCESS);
    EXPECT_EQ(vortex_init(guard.loop, "Second"), VORTEX_ERR_ALREADY_INIT);
}

TEST(CApiTest, NullGuards)
{
    EXPECT_EQ(vortex_create(nullptr), VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_init(nullptr, "x"), VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_run(nullptr), VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_post(nullptr, nullptr, nullptr), VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_add_source(nullptr, -1, nullptr, nullptr), VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_is_running(nullptr), 0);
    EXPECT_EQ(vortex_name(nullptr), nullptr);
    vortex_stop(nullptr);
    vortex_remove_source(nullptr, -1);
    vortex_destroy(nullptr);
}

TEST(CApiTest, RunStop)
{
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "RunStop"), VORTEX_SUCCESS);

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });

    for (int i = 0; i < 100 && !vortex_is_running(guard.loop); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(vortex_is_running(guard.loop), 1);
    vortex_stop(guard.loop);
    t.join();
    EXPECT_EQ(vortex_is_running(guard.loop), 0);
}

TEST(CApiTest, PostCallable)
{
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "PostCallable"), VORTEX_SUCCESS);

    std::thread::id loopThreadId;
    std::thread::id callbackThreadId;
    std::atomic<bool> done{false};

    std::thread t([&] {
        loopThreadId = std::this_thread::get_id();
        EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS);
    });

    std::this_thread::sleep_for(10ms);

    auto callback = [](void *userData) {
        auto *ctx = static_cast<std::pair<std::thread::id *, std::atomic<bool> *> *>(userData);
        *ctx->first = std::this_thread::get_id();
        ctx->second->store(true);
    };

    std::pair<std::thread::id *, std::atomic<bool> *> ctx{&callbackThreadId, &done};
    ASSERT_EQ(vortex_post(guard.loop, callback, &ctx), VORTEX_SUCCESS);
    ASSERT_EQ(vortex_post(guard.loop, stopLoop, guard.loop), VORTEX_SUCCESS);
    t.join();

    EXPECT_TRUE(done.load());
    EXPECT_EQ(callbackThreadId, loopThreadId);
}

TEST(CApiTest, PostUserData)
{
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "PostData"), VORTEX_SUCCESS);

    std::atomic<int> value{0};
    auto setValue = [](void *userData) {
        static_cast<std::atomic<int> *>(userData)->store(42);
    };

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });
    std::this_thread::sleep_for(10ms);

    ASSERT_EQ(vortex_post(guard.loop, setValue, &value), VORTEX_SUCCESS);
    ASSERT_EQ(vortex_post(guard.loop, stopLoop, guard.loop), VORTEX_SUCCESS);
    t.join();

    EXPECT_EQ(value.load(), 42);
}

TEST(CApiTest, AddRemoveSource)
{
#if defined(_WIN32)
    GTEST_SKIP() << "fd-based C API source test is Unix-only";
#else
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "AddRemove"), VORTEX_SUCCESS);
    auto [readFd, writeFd] = makePipe();

    std::atomic<int> count{0};
    auto onReadable = [](void *userData) {
        auto *ctx = static_cast<std::pair<Handle, std::atomic<int> *> *>(userData);
        drainPipe(ctx->first);
        ctx->second->fetch_add(1);
    };
    std::pair<Handle, std::atomic<int> *> ctx{readFd, &count};

    ASSERT_EQ(vortex_add_source(guard.loop, readFd, onReadable, &ctx), VORTEX_SUCCESS);

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });
    std::this_thread::sleep_for(10ms);

    writeByte(writeFd);
    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    vortex_remove_source(guard.loop, readFd);
    writeByte(writeFd);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count.load(), 1);

    vortex_stop(guard.loop);
    t.join();
    closePipe(readFd, writeFd);
#endif
}

TEST(CApiTest, SourceCallback)
{
#if defined(_WIN32)
    GTEST_SKIP() << "fd-based C API source test is Unix-only";
#else
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "SourceCallback"), VORTEX_SUCCESS);
    auto [readFd, writeFd] = makePipe();

    std::atomic<bool> fired{false};
    auto onReadable = [](void *userData) {
        auto *ctx = static_cast<std::pair<Handle, std::atomic<bool> *> *>(userData);
        drainPipe(ctx->first);
        ctx->second->store(true);
    };
    std::pair<Handle, std::atomic<bool> *> ctx{readFd, &fired};

    ASSERT_EQ(vortex_add_source(guard.loop, readFd, onReadable, &ctx), VORTEX_SUCCESS);

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });
    std::this_thread::sleep_for(10ms);

    writeByte(writeFd);
    for (int i = 0; i < 200 && !fired.load(); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_TRUE(fired.load());

    vortex_remove_source(guard.loop, readFd);
    vortex_stop(guard.loop);
    t.join();
    closePipe(readFd, writeFd);
#endif
}

TEST(CApiTest, DestroyStops)
{
    vortex_t loop = nullptr;
    ASSERT_EQ(vortex_create(&loop), VORTEX_SUCCESS);
    ASSERT_EQ(vortex_init(loop, "DestroyStops"), VORTEX_SUCCESS);

    std::thread t([&] { EXPECT_EQ(vortex_run(loop), VORTEX_SUCCESS); });
    for (int i = 0; i < 100 && !vortex_is_running(loop); ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(vortex_is_running(loop), 1);
    vortex_destroy(loop);
    t.join();
}

TEST(CApiTest, FreeNull)
{
    vortex_free(nullptr);
    SUCCEED();
}

TEST(CApiTest, OneShotTimer)
{
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "CTimer"), VORTEX_SUCCESS);

    std::atomic<int> count{0};
    auto cb = [](void *userData) {
        static_cast<std::atomic<int> *>(userData)->fetch_add(1);
    };

    uint64_t timerId = 0;
    ASSERT_EQ(vortex_add_timer(guard.loop, 20, 0, cb, &count, &timerId), VORTEX_SUCCESS);
    EXPECT_GE(timerId, 1u);

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });

    for (int i = 0; i < 200 && count.load() < 1; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_EQ(count.load(), 1);

    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(count.load(), 1);

    vortex_stop(guard.loop);
    t.join();
}

TEST(CApiTest, RepeatingTimer)
{
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "CRepeat"), VORTEX_SUCCESS);

    std::atomic<int> count{0};
    auto cb = [](void *userData) {
        static_cast<std::atomic<int> *>(userData)->fetch_add(1);
    };

    uint64_t timerId = 0;
    ASSERT_EQ(vortex_add_timer(guard.loop, 15, 1, cb, &count, &timerId), VORTEX_SUCCESS);

    std::thread t([&] { EXPECT_EQ(vortex_run(guard.loop), VORTEX_SUCCESS); });

    for (int i = 0; i < 400 && count.load() < 3; ++i)
        std::this_thread::sleep_for(5ms);

    EXPECT_GE(count.load(), 3);

    vortex_remove_timer(guard.loop, timerId);
    vortex_stop(guard.loop);
    t.join();
}

TEST(CApiTest, TimerNullGuards)
{
    EXPECT_EQ(vortex_add_timer(nullptr, 100, 0, nullptr, nullptr, nullptr),
              VORTEX_ERR_INVALID_ARGUMENT);
    vortex_remove_timer(nullptr, 1);

    VortexGuard guard;
    uint64_t timerId = 0;
    EXPECT_EQ(vortex_add_timer(guard.loop, 100, 0, [](void *) {}, nullptr, &timerId),
              VORTEX_ERR_NOT_INIT);
}

// ---------- Source Error Callback C API Tests ----------

TEST(CApiTest, AddSourceWithErrorNullGuards)
{
    EXPECT_EQ(vortex_add_source_with_error(nullptr, 0, [](void *) {}, nullptr,
                                            [](void *) {}, nullptr),
              VORTEX_ERR_INVALID_ARGUMENT);

    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "ErrorNullGuards"), VORTEX_SUCCESS);
    EXPECT_EQ(vortex_add_source_with_error(guard.loop, -1, [](void *) {}, nullptr,
                                            [](void *) {}, nullptr),
              VORTEX_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(vortex_add_source_with_error(guard.loop, 0, nullptr, nullptr,
                                            [](void *) {}, nullptr),
              VORTEX_ERR_INVALID_ARGUMENT);
}

TEST(CApiTest, AddSourceWithErrorCallback)
{
#if defined(_WIN32)
    GTEST_SKIP() << "CreatePipe handles are not waitable by WaitForMultipleObjects";
#else
    VortexGuard guard;
    ASSERT_EQ(vortex_init(guard.loop, "ErrorCallback"), VORTEX_SUCCESS);

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    std::atomic<bool> errorFired{false};

    EXPECT_EQ(vortex_add_source_with_error(
                  guard.loop, fds[0],
                  [](void *) {},
                  nullptr,
                  [](void *ctx) { static_cast<std::atomic<bool> *>(ctx)->store(true); },
                  &errorFired),
              VORTEX_SUCCESS);

    std::thread t([&] { vortex_run(guard.loop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    close(fds[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(errorFired.load());

    vortex_stop(guard.loop);
    t.join();
    close(fds[0]);
#endif
}
