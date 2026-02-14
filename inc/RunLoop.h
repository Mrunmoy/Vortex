#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ms
{

    // Pure event loop. Runs on a dedicated thread, allows other
    // components to post work to that thread. No transport knowledge.
    //
    // Usage:
    //   RunLoop loop;
    //   loop.init("MyApp");
    //   loop.executeOnRunLoop([&] { /* runs on loop thread */ });
    //   loop.run();  // blocks until stop()

    class RunLoop
    {
    public:
        struct Version
        {
            static constexpr uint8_t major = 1;
            static constexpr uint8_t minor = 0;
            static constexpr uint8_t patch = 0;
            static constexpr uint32_t packed = (major << 16) | (minor << 8) | patch;
        };

        RunLoop();
        ~RunLoop();

        RunLoop(const RunLoop &) = delete;
        RunLoop &operator=(const RunLoop &) = delete;

        // Initialize the run loop. `name` identifies this loop
        // for debugging/logging purposes.
        void init(const char *name);

        // Block the calling thread, dispatching events until stop() is called.
        void run();

        // Signal the run loop to exit. Thread-safe, callable from any thread
        // or from within a posted callable.
        void stop();

        // Post a callable to be executed on the run loop thread.
        // Thread-safe — can be called from any thread.
        void executeOnRunLoop(std::function<void()> fn);

        bool isRunning() const { return m_running.load(std::memory_order_acquire); }
        const char *name() const { return m_name; }

    private:
        void wakeup();

        const char *m_name = "";
        int m_epollFd = -1;
        int m_wakeupFd[2] = {-1, -1};

        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};

        std::mutex m_postMutex;
        std::vector<std::function<void()>> m_postQueue;
    };

} // namespace ms
