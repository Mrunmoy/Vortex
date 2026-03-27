// ── Competitive benchmarks: Vortex vs libuv vs libevent vs Boost.Asio ──
//
// Build:  python3 build.py -b
// Run:    ./build/bench/vortex_benchmarks

#include <benchmark/benchmark.h>

#include <RunLoop.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include <unistd.h>

#ifdef HAS_LIBUV
#include <uv.h>
#endif

#ifdef HAS_LIBEVENT
#include <event2/event.h>
#include <event2/thread.h>
#endif

#ifdef HAS_BOOST_ASIO
#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#endif

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

// ════════════════════════════════════════════════════════════════════════
// A)  Post-to-execute latency (cross-thread)
// ════════════════════════════════════════════════════════════════════════

static void BM_Vortex_PostLatency(benchmark::State& state)
{
    for (auto _ : state) {
        vortex::RunLoop loop;
        loop.init("bench");

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            loop.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Small delay to ensure loop is actually polling
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        constexpr int kBatch = 64;
        std::atomic<int> done{0};
        double totalNs = 0.0;
        std::mutex mu;

        for (int i = 0; i < kBatch; ++i) {
            auto sendTime = Clock::now();
            loop.executeOnRunLoop([&, sendTime] {
                auto dt = Clock::now() - sendTime;
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                {
                    std::lock_guard<std::mutex> lk(mu);
                    totalNs += ns;
                }
                done.fetch_add(1, std::memory_order_release);
            });
            // Small gap so we don't overwhelm
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        while (done.load(std::memory_order_acquire) < kBatch) {
            std::this_thread::yield();
        }

        loop.stop();
        t.join();

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["unit"] = benchmark::Counter(0, benchmark::Counter::kDefaults, benchmark::Counter::kIs1000);
}

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBUV
static void BM_Libuv_PostLatency(benchmark::State& state)
{
    for (auto _ : state) {
        uv_loop_t* loop = uv_loop_new();

        struct AsyncCtx {
            std::mutex mu;
            std::vector<std::function<void()>> queue;
            bool closing = false;
        } ctx;

        uv_async_t async;
        async.data = &ctx;
        uv_async_init(loop, &async, [](uv_async_t* h) {
            auto* c = static_cast<AsyncCtx*>(h->data);
            std::vector<std::function<void()>> batch;
            bool shouldClose = false;
            {
                std::lock_guard<std::mutex> lk(c->mu);
                batch.swap(c->queue);
                shouldClose = c->closing;
            }
            for (auto& fn : batch) fn();
            if (shouldClose) {
                uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
            }
        });

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            uv_run(loop, UV_RUN_DEFAULT);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        constexpr int kBatch = 64;
        std::atomic<int> done{0};
        double totalNs = 0.0;
        std::mutex mu;

        for (int i = 0; i < kBatch; ++i) {
            auto sendTime = Clock::now();
            {
                std::lock_guard<std::mutex> lk(ctx.mu);
                ctx.queue.push_back([&, sendTime] {
                    auto dt = Clock::now() - sendTime;
                    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                    {
                        std::lock_guard<std::mutex> lk2(mu);
                        totalNs += ns;
                    }
                    done.fetch_add(1, std::memory_order_release);
                });
            }
            uv_async_send(&async);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        while (done.load(std::memory_order_acquire) < kBatch) {
            std::this_thread::yield();
        }

        // Signal close from loop thread — loop exits when no handles remain
        {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.closing = true;
        }
        uv_async_send(&async);
        t.join();
        uv_loop_close(loop);
        free(loop);

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBEVENT
namespace {
struct LibeventInit {
    LibeventInit() { evthread_use_pthreads(); }
};
static LibeventInit sLibeventInit;
} // namespace

static void BM_Libevent_PostLatency(benchmark::State& state)
{
    for (auto _ : state) {
        event_base* base = event_base_new();

        // Cross-thread wakeup via socketpair
        int spair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, spair) != 0) {
            state.SkipWithError("socketpair failed");
            return;
        }

        struct PostCtx {
            std::mutex mu;
            std::vector<std::function<void()>> queue;
        } ctx;

        event* ev = event_new(base, spair[0], EV_READ | EV_PERSIST,
            [](evutil_socket_t fd, short, void* arg) {
                char buf[64];
                (void)read(fd, buf, sizeof(buf));
                auto* c = static_cast<PostCtx*>(arg);
                std::vector<std::function<void()>> batch;
                {
                    std::lock_guard<std::mutex> lk(c->mu);
                    batch.swap(c->queue);
                }
                for (auto& fn : batch) fn();
            }, &ctx);
        event_add(ev, nullptr);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            event_base_dispatch(base);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        constexpr int kBatch = 64;
        std::atomic<int> done{0};
        double totalNs = 0.0;
        std::mutex mu;

        for (int i = 0; i < kBatch; ++i) {
            auto sendTime = Clock::now();
            {
                std::lock_guard<std::mutex> lk(ctx.mu);
                ctx.queue.push_back([&, sendTime] {
                    auto dt = Clock::now() - sendTime;
                    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                    {
                        std::lock_guard<std::mutex> lk2(mu);
                        totalNs += ns;
                    }
                    done.fetch_add(1, std::memory_order_release);
                });
            }
            char c = 'x';
            (void)write(spair[1], &c, 1);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        while (done.load(std::memory_order_acquire) < kBatch) {
            std::this_thread::yield();
        }

        event_base_loopexit(base, nullptr);
        t.join();
        event_free(ev);
        event_base_free(base);
        close(spair[0]);
        close(spair[1]);

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_BOOST_ASIO
static void BM_Asio_PostLatency(benchmark::State& state)
{
    for (auto _ : state) {
        boost::asio::io_context io;
        auto work = boost::asio::make_work_guard(io);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            io.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        constexpr int kBatch = 64;
        std::atomic<int> done{0};
        double totalNs = 0.0;
        std::mutex mu;

        for (int i = 0; i < kBatch; ++i) {
            auto sendTime = Clock::now();
            boost::asio::post(io, [&, sendTime] {
                auto dt = Clock::now() - sendTime;
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
                {
                    std::lock_guard<std::mutex> lk(mu);
                    totalNs += ns;
                }
                done.fetch_add(1, std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        while (done.load(std::memory_order_acquire) < kBatch) {
            std::this_thread::yield();
        }

        work.reset();
        io.stop();
        t.join();

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ════════════════════════════════════════════════════════════════════════
// B)  Post throughput (single-producer)
// ════════════════════════════════════════════════════════════════════════

static void BM_Vortex_PostThroughput(benchmark::State& state)
{
    vortex::RunLoop loop;
    loop.init("bench");

    std::atomic<bool> ready{false};
    std::thread t([&] {
        ready.store(true, std::memory_order_release);
        loop.run();
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<int64_t> count{0};

    for (auto _ : state) {
        loop.executeOnRunLoop([&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Drain — post a final sentinel
    std::atomic<bool> flushed{false};
    loop.executeOnRunLoop([&] { flushed.store(true, std::memory_order_release); });
    while (!flushed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    loop.stop();
    t.join();

    state.SetItemsProcessed(state.iterations());
}

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBUV
static void BM_Libuv_PostThroughput(benchmark::State& state)
{
    uv_loop_t* loop = uv_loop_new();

    struct AsyncCtx {
        std::mutex mu;
        std::vector<std::function<void()>> queue;
        bool closing = false;
    } ctx;

    uv_async_t async;
    async.data = &ctx;
    uv_async_init(loop, &async, [](uv_async_t* h) {
        auto* c = static_cast<AsyncCtx*>(h->data);
        std::vector<std::function<void()>> batch;
        bool shouldClose = false;
        {
            std::lock_guard<std::mutex> lk(c->mu);
            batch.swap(c->queue);
            shouldClose = c->closing;
        }
        for (auto& fn : batch) fn();
        if (shouldClose) {
            uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
        }
    });

    std::atomic<bool> ready{false};
    std::thread t([&] {
        ready.store(true, std::memory_order_release);
        uv_run(loop, UV_RUN_DEFAULT);
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<int64_t> count{0};

    for (auto _ : state) {
        {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.queue.push_back([&] {
                count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        uv_async_send(&async);
    }

    // Flush then close
    std::atomic<bool> flushed{false};
    {
        std::lock_guard<std::mutex> lk(ctx.mu);
        ctx.queue.push_back([&] { flushed.store(true, std::memory_order_release); });
    }
    uv_async_send(&async);
    while (!flushed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    {
        std::lock_guard<std::mutex> lk(ctx.mu);
        ctx.closing = true;
    }
    uv_async_send(&async);
    t.join();
    uv_loop_close(loop);
    free(loop);

    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBEVENT
static void BM_Libevent_PostThroughput(benchmark::State& state)
{
    event_base* base = event_base_new();

    int spair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, spair) != 0) {
        state.SkipWithError("socketpair failed");
        return;
    }

    struct PostCtx {
        std::mutex mu;
        std::vector<std::function<void()>> queue;
    } ctx;

    event* ev = event_new(base, spair[0], EV_READ | EV_PERSIST,
        [](evutil_socket_t fd, short, void* arg) {
            char buf[256];
            (void)read(fd, buf, sizeof(buf));
            auto* c = static_cast<PostCtx*>(arg);
            std::vector<std::function<void()>> batch;
            {
                std::lock_guard<std::mutex> lk(c->mu);
                batch.swap(c->queue);
            }
            for (auto& fn : batch) fn();
        }, &ctx);
    event_add(ev, nullptr);

    std::atomic<bool> ready{false};
    std::thread t([&] {
        ready.store(true, std::memory_order_release);
        event_base_dispatch(base);
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<int64_t> count{0};

    for (auto _ : state) {
        {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.queue.push_back([&] {
                count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        char c = 'x';
        (void)write(spair[1], &c, 1);
    }

    // Flush
    std::atomic<bool> flushed{false};
    {
        std::lock_guard<std::mutex> lk(ctx.mu);
        ctx.queue.push_back([&] { flushed.store(true, std::memory_order_release); });
    }
    {
        char c = 'x';
        (void)write(spair[1], &c, 1);
    }
    while (!flushed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    event_base_loopexit(base, nullptr);
    t.join();
    event_free(ev);
    event_base_free(base);
    close(spair[0]);
    close(spair[1]);

    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_BOOST_ASIO
static void BM_Asio_PostThroughput(benchmark::State& state)
{
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);

    std::atomic<bool> ready{false};
    std::thread t([&] {
        ready.store(true, std::memory_order_release);
        io.run();
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<int64_t> count{0};

    for (auto _ : state) {
        boost::asio::post(io, [&] {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Flush
    std::atomic<bool> flushed{false};
    boost::asio::post(io, [&] { flushed.store(true, std::memory_order_release); });
    while (!flushed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    work.reset();
    io.stop();
    t.join();

    state.SetItemsProcessed(state.iterations());
}
#endif

// ════════════════════════════════════════════════════════════════════════
// C)  Timer jitter
// ════════════════════════════════════════════════════════════════════════

static constexpr int kTimerSamples = 100;
static constexpr uint32_t kTimerIntervalMs = 1;

static double computeMeanJitterNs(const std::vector<TimePoint>& stamps, uint32_t intervalMs)
{
    if (stamps.size() < 2) return 0.0;
    double expectedNs = intervalMs * 1e6;
    double totalJitter = 0.0;
    for (size_t i = 1; i < stamps.size(); ++i) {
        double dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
            stamps[i] - stamps[i - 1]).count();
        totalJitter += std::abs(dt - expectedNs);
    }
    return totalJitter / static_cast<double>(stamps.size() - 1);
}

static void BM_Vortex_TimerJitter(benchmark::State& state)
{
    for (auto _ : state) {
        vortex::RunLoop loop;
        loop.init("bench");

        std::mutex mu;
        std::vector<TimePoint> stamps;
        stamps.reserve(kTimerSamples);

        auto tid = loop.addTimer(kTimerIntervalMs, true, [&] {
            std::lock_guard<std::mutex> lk(mu);
            stamps.push_back(Clock::now());
        });

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            loop.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Wait for enough samples
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (static_cast<int>(stamps.size()) >= kTimerSamples) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        loop.removeTimer(tid);
        loop.stop();
        t.join();

        std::lock_guard<std::mutex> lk(mu);
        double jitter = computeMeanJitterNs(stamps, kTimerIntervalMs);
        state.SetIterationTime(jitter / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["jitter_ns"] = benchmark::Counter(0, benchmark::Counter::kAvgIterations);
}

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBUV
static void BM_Libuv_TimerJitter(benchmark::State& state)
{
    for (auto _ : state) {
        uv_loop_t* loop = uv_loop_new();

        struct TimerCtx {
            std::mutex mu;
            std::vector<TimePoint> stamps;
        } ctx;
        ctx.stamps.reserve(kTimerSamples);

        uv_timer_t timer;
        timer.data = &ctx;
        uv_timer_init(loop, &timer);
        uv_timer_start(&timer, [](uv_timer_t* h) {
            auto* c = static_cast<TimerCtx*>(h->data);
            std::lock_guard<std::mutex> lk(c->mu);
            c->stamps.push_back(Clock::now());
        }, kTimerIntervalMs, kTimerIntervalMs);

        // Keep-alive async handle for controlled shutdown
        uv_async_t stopAsync;
        stopAsync.data = &timer;
        uv_async_init(loop, &stopAsync, [](uv_async_t* h) {
            auto* tmr = static_cast<uv_timer_t*>(h->data);
            uv_timer_stop(tmr);
            uv_close(reinterpret_cast<uv_handle_t*>(tmr), nullptr);
            uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
        });

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            uv_run(loop, UV_RUN_DEFAULT);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lk(ctx.mu);
                if (static_cast<int>(ctx.stamps.size()) >= kTimerSamples) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        uv_async_send(&stopAsync);
        t.join();
        uv_loop_close(loop);
        free(loop);

        std::lock_guard<std::mutex> lk(ctx.mu);
        double jitter = computeMeanJitterNs(ctx.stamps, kTimerIntervalMs);
        state.SetIterationTime(jitter / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBEVENT
static void BM_Libevent_TimerJitter(benchmark::State& state)
{
    for (auto _ : state) {
        event_base* base = event_base_new();

        struct TimerCtx {
            event_base* base;
            event* ev;
            std::mutex mu;
            std::vector<TimePoint> stamps;
        } ctx;
        ctx.base = base;
        ctx.stamps.reserve(kTimerSamples);

        auto timer_cb = [](evutil_socket_t, short, void* arg) {
            auto* c = static_cast<TimerCtx*>(arg);
            {
                std::lock_guard<std::mutex> lk(c->mu);
                c->stamps.push_back(Clock::now());
            }
            // Re-arm the timer for repeating behaviour
            struct timeval tv = {0, kTimerIntervalMs * 1000};
            evtimer_add(c->ev, &tv);
        };
        ctx.ev = evtimer_new(base, timer_cb, &ctx);

        struct timeval tv = {0, static_cast<long>(kTimerIntervalMs * 1000)};
        evtimer_add(ctx.ev, &tv);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            event_base_dispatch(base);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lk(ctx.mu);
                if (static_cast<int>(ctx.stamps.size()) >= kTimerSamples) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        event_base_loopexit(base, nullptr);
        t.join();
        event_free(ctx.ev);
        event_base_free(base);

        std::lock_guard<std::mutex> lk(ctx.mu);
        double jitter = computeMeanJitterNs(ctx.stamps, kTimerIntervalMs);
        state.SetIterationTime(jitter / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_BOOST_ASIO
static void BM_Asio_TimerJitter(benchmark::State& state)
{
    for (auto _ : state) {
        boost::asio::io_context io;
        auto work = boost::asio::make_work_guard(io);

        std::mutex mu;
        std::vector<TimePoint> stamps;
        stamps.reserve(kTimerSamples);

        boost::asio::steady_timer timer(io);

        std::function<void(boost::system::error_code)> onFire;
        onFire = [&](boost::system::error_code ec) {
            if (ec) return;
            {
                std::lock_guard<std::mutex> lk(mu);
                stamps.push_back(Clock::now());
            }
            timer.expires_after(std::chrono::milliseconds(kTimerIntervalMs));
            timer.async_wait(onFire);
        };

        timer.expires_after(std::chrono::milliseconds(kTimerIntervalMs));
        timer.async_wait(onFire);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            io.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (static_cast<int>(stamps.size()) >= kTimerSamples) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        timer.cancel();
        work.reset();
        io.stop();
        t.join();

        std::lock_guard<std::mutex> lk(mu);
        double jitter = computeMeanJitterNs(stamps, kTimerIntervalMs);
        state.SetIterationTime(jitter / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ════════════════════════════════════════════════════════════════════════
// D)  FD source reaction time
// ════════════════════════════════════════════════════════════════════════

static void BM_Vortex_FdReaction(benchmark::State& state)
{
    for (auto _ : state) {
        int pipefd[2];
        if (pipe(pipefd) != 0) { state.SkipWithError("pipe failed"); return; }

        vortex::RunLoop loop;
        loop.init("bench");

        std::atomic<bool> ready{false};
        double totalNs = 0.0;
        constexpr int kBatch = 64;
        std::atomic<int> done{0};
        std::mutex mu;

        loop.addSource(pipefd[0], [&] {
            auto arriveTime = Clock::now();
            char buf[64];
            (void)read(pipefd[0], buf, sizeof(buf));
            // Read the timestamp from our tracking
            {
                std::lock_guard<std::mutex> lk(mu);
                // We stored the send time in a shared variable
            }
            benchmark::DoNotOptimize(arriveTime);
        });

        // Simpler approach: measure round-trip from write to handler
        // We'll use a shared variable for send timestamps
        std::vector<TimePoint> sendTimes(kBatch);
        std::atomic<int> rxCount{0};

        loop.removeSource(pipefd[0]);
        loop.addSource(pipefd[0], [&] {
            auto arriveTime = Clock::now();
            char buf[1];
            (void)read(pipefd[0], buf, sizeof(buf));
            int idx = rxCount.fetch_add(1, std::memory_order_acq_rel);
            if (idx < kBatch) {
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    arriveTime - sendTimes[idx]).count();
                std::lock_guard<std::mutex> lk(mu);
                totalNs += ns;
                done.fetch_add(1, std::memory_order_release);
            }
        });

        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            loop.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        for (int i = 0; i < kBatch; ++i) {
            sendTimes[i] = Clock::now();
            char c = 'x';
            (void)write(pipefd[1], &c, 1);
            // Wait for this one to be processed before sending next
            while (done.load(std::memory_order_acquire) <= i) {
                std::this_thread::yield();
            }
        }

        loop.stop();
        t.join();
        close(pipefd[0]);
        close(pipefd[1]);

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBUV
static void BM_Libuv_FdReaction(benchmark::State& state)
{
    for (auto _ : state) {
        int pipefd[2];
        if (pipe(pipefd) != 0) { state.SkipWithError("pipe failed"); return; }

        uv_loop_t* loop = uv_loop_new();

        constexpr int kBatch = 64;
        struct PollCtx {
            int readFd;
            std::vector<TimePoint> sendTimes;
            std::atomic<int> rxCount{0};
            std::atomic<int> done{0};
            std::mutex mu;
            double totalNs = 0.0;
        } ctx;
        ctx.readFd = pipefd[0];
        ctx.sendTimes.resize(kBatch);

        uv_poll_t poll;
        poll.data = &ctx;
        uv_poll_init(loop, &poll, pipefd[0]);
        uv_poll_start(&poll, UV_READABLE, [](uv_poll_t* h, int, int) {
            auto arriveTime = Clock::now();
            auto* c = static_cast<PollCtx*>(h->data);
            char buf[1];
            (void)read(c->readFd, buf, sizeof(buf));
            int idx = c->rxCount.fetch_add(1, std::memory_order_acq_rel);
            if (idx < static_cast<int>(c->sendTimes.size())) {
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    arriveTime - c->sendTimes[idx]).count();
                std::lock_guard<std::mutex> lk(c->mu);
                c->totalNs += ns;
                c->done.fetch_add(1, std::memory_order_release);
            }
        });

        // Stop async for clean shutdown from loop thread
        uv_async_t stopAsync;
        stopAsync.data = &poll;
        uv_async_init(loop, &stopAsync, [](uv_async_t* h) {
            auto* p = static_cast<uv_poll_t*>(h->data);
            uv_poll_stop(p);
            uv_close(reinterpret_cast<uv_handle_t*>(p), nullptr);
            uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
        });

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            uv_run(loop, UV_RUN_DEFAULT);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        for (int i = 0; i < kBatch; ++i) {
            ctx.sendTimes[i] = Clock::now();
            char c = 'x';
            (void)write(pipefd[1], &c, 1);
            while (ctx.done.load(std::memory_order_acquire) <= i) {
                std::this_thread::yield();
            }
        }

        uv_async_send(&stopAsync);
        t.join();
        uv_loop_close(loop);
        free(loop);
        close(pipefd[0]);
        close(pipefd[1]);

        state.SetIterationTime(ctx.totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_LIBEVENT
static void BM_Libevent_FdReaction(benchmark::State& state)
{
    for (auto _ : state) {
        int pipefd[2];
        if (pipe(pipefd) != 0) { state.SkipWithError("pipe failed"); return; }

        event_base* base = event_base_new();

        constexpr int kBatch = 64;
        struct FdCtx {
            std::vector<TimePoint> sendTimes;
            std::atomic<int> rxCount{0};
            std::atomic<int> done{0};
            std::mutex mu;
            double totalNs = 0.0;
        } ctx;
        ctx.sendTimes.resize(kBatch);

        event* ev = event_new(base, pipefd[0], EV_READ | EV_PERSIST,
            [](evutil_socket_t fd, short, void* arg) {
                auto arriveTime = Clock::now();
                auto* c = static_cast<FdCtx*>(arg);
                char buf[1];
                (void)read(fd, buf, sizeof(buf));
                int idx = c->rxCount.fetch_add(1, std::memory_order_acq_rel);
                if (idx < static_cast<int>(c->sendTimes.size())) {
                    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        arriveTime - c->sendTimes[idx]).count();
                    std::lock_guard<std::mutex> lk(c->mu);
                    c->totalNs += ns;
                    c->done.fetch_add(1, std::memory_order_release);
                }
            }, &ctx);
        event_add(ev, nullptr);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            event_base_dispatch(base);
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        for (int i = 0; i < kBatch; ++i) {
            ctx.sendTimes[i] = Clock::now();
            char c = 'x';
            (void)write(pipefd[1], &c, 1);
            while (ctx.done.load(std::memory_order_acquire) <= i) {
                std::this_thread::yield();
            }
        }

        event_base_loopexit(base, nullptr);
        t.join();
        event_free(ev);
        event_base_free(base);
        close(pipefd[0]);
        close(pipefd[1]);

        state.SetIterationTime(ctx.totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ────────────────────────────────────────────────────────────────────────

#ifdef HAS_BOOST_ASIO
static void BM_Asio_FdReaction(benchmark::State& state)
{
    for (auto _ : state) {
        int pipefd[2];
        if (pipe(pipefd) != 0) { state.SkipWithError("pipe failed"); return; }

        boost::asio::io_context io;
        auto work = boost::asio::make_work_guard(io);
        boost::asio::posix::stream_descriptor sd(io, pipefd[0]);

        constexpr int kBatch = 64;
        std::vector<TimePoint> sendTimes(kBatch);
        std::atomic<int> rxCount{0};
        std::atomic<int> done{0};
        std::mutex mu;
        double totalNs = 0.0;

        std::function<void(boost::system::error_code)> waitFn;
        waitFn = [&](boost::system::error_code ec) {
            if (ec) return;
            auto arriveTime = Clock::now();
            char buf[1];
            (void)::read(sd.native_handle(), buf, sizeof(buf));
            int idx = rxCount.fetch_add(1, std::memory_order_acq_rel);
            if (idx < kBatch) {
                double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    arriveTime - sendTimes[idx]).count();
                std::lock_guard<std::mutex> lk(mu);
                totalNs += ns;
                done.fetch_add(1, std::memory_order_release);
            }
            sd.async_wait(boost::asio::posix::stream_descriptor::wait_read, waitFn);
        };
        sd.async_wait(boost::asio::posix::stream_descriptor::wait_read, waitFn);

        std::atomic<bool> ready{false};
        std::thread t([&] {
            ready.store(true, std::memory_order_release);
            io.run();
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));

        for (int i = 0; i < kBatch; ++i) {
            sendTimes[i] = Clock::now();
            char c = 'x';
            (void)::write(pipefd[1], &c, 1);
            while (done.load(std::memory_order_acquire) <= i) {
                std::this_thread::yield();
            }
        }

        sd.cancel();
        // Release the fd so we close it ourselves
        sd.release();
        work.reset();
        io.stop();
        t.join();
        close(pipefd[0]);
        close(pipefd[1]);

        state.SetIterationTime(totalNs / kBatch / 1e9);
    }
    state.SetItemsProcessed(state.iterations());
}
#endif

// ════════════════════════════════════════════════════════════════════════
// Registration
// ════════════════════════════════════════════════════════════════════════

// A) Post latency — heavyweight (thread per iteration), cap iterations
BENCHMARK(BM_Vortex_PostLatency)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#ifdef HAS_LIBUV
BENCHMARK(BM_Libuv_PostLatency)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif
#ifdef HAS_LIBEVENT
BENCHMARK(BM_Libevent_PostLatency)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif
#ifdef HAS_BOOST_ASIO
BENCHMARK(BM_Asio_PostLatency)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif

// B) Post throughput
BENCHMARK(BM_Vortex_PostThroughput);
#ifdef HAS_LIBUV
BENCHMARK(BM_Libuv_PostThroughput);
#endif
#ifdef HAS_LIBEVENT
BENCHMARK(BM_Libevent_PostThroughput);
#endif
#ifdef HAS_BOOST_ASIO
BENCHMARK(BM_Asio_PostThroughput);
#endif

// C) Timer jitter — heavyweight, cap iterations
BENCHMARK(BM_Vortex_TimerJitter)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(5);
#ifdef HAS_LIBUV
BENCHMARK(BM_Libuv_TimerJitter)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(5);
#endif
#ifdef HAS_LIBEVENT
BENCHMARK(BM_Libevent_TimerJitter)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(5);
#endif
#ifdef HAS_BOOST_ASIO
BENCHMARK(BM_Asio_TimerJitter)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(5);
#endif

// D) FD reaction — heavyweight, cap iterations
BENCHMARK(BM_Vortex_FdReaction)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#ifdef HAS_LIBUV
BENCHMARK(BM_Libuv_FdReaction)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif
#ifdef HAS_LIBEVENT
BENCHMARK(BM_Libevent_FdReaction)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif
#ifdef HAS_BOOST_ASIO
BENCHMARK(BM_Asio_FdReaction)->UseManualTime()->Unit(benchmark::kNanosecond)->Iterations(10);
#endif

BENCHMARK_MAIN();
