#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vortex
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
#if defined(_WIN32)
        using NativeHandle = void *;
#else
        using NativeHandle = int;
#endif

        struct Version
        {
            static constexpr uint8_t major = 1;
            static constexpr uint8_t minor = 1;
            static constexpr uint8_t patch = 0;
            static constexpr uint32_t packed = (major << 16) | (minor << 8) | patch;
        };

        RunLoop();
        ~RunLoop();

        RunLoop(const RunLoop &) = delete;
        RunLoop &operator=(const RunLoop &) = delete;

        // Initialize the run loop. Must be called exactly once per instance.
        // `name` identifies this loop for debugging/logging purposes.
        // Throws std::logic_error if already initialized.
        // Throws std::system_error on failure (platform event setup).
        void init(const char *name);

        // Block the calling thread, dispatching events until stop() is called.
        // Throws std::system_error on poll failure. Propagates any
        // exception thrown by a posted callable or source handler.
        // Flags are reset on all exit paths (the loop is restartable).
        void run();

        // Signal the run loop to exit. Thread-safe, callable from any thread
        // or from within a posted callable.
        void stop();

        // Post a callable to be executed on the run loop thread.
        // Thread-safe — can be called from any thread.
        void executeOnRunLoop(std::function<void()> fn);

        // Watch a native handle. When signalled, `handler` is called on the
        // run loop thread. If `handle` is already watched, replaces the
        // existing handler. Thread-safe — can be called from any thread.
        // Throws std::system_error on failure.
        //
        // Backend-specific:
        //   epoll/kqueue: monitors fd for readability (EPOLLIN / EVFILT_READ)
        //   win32: handle must be a waitable kernel object (event, semaphore,
        //          etc.); pipes/sockets require overlapped I/O instead.
        //          Max 63 sources (MAXIMUM_WAIT_OBJECTS - 1).
        //   stub: handler is stored but never fired (no OS polling).
        void addSource(NativeHandle handle, std::function<void()> handler);

        // Stop watching a native handle. Thread-safe.
        // Throws std::system_error on unexpected failure.
        void removeSource(NativeHandle handle);

        bool isRunning() const { return m_running.load(std::memory_order_acquire); }
        const char *name() const { return m_name.c_str(); }

    private:
        void wakeup();

        std::string m_name;

        // Platform-specific handles
#if defined(__linux__)
        int m_pollFd = -1;
        int m_wakeupFd[2] = {-1, -1};
#elif defined(__APPLE__)
        int m_pollFd = -1;
        int m_wakeupFd[2] = {-1, -1};
#elif defined(_WIN32)
        void *m_pollHandle = nullptr;
        void *m_wakeupHandle = nullptr;
#else
        int m_wakeupFd[2] = {-1, -1};
#endif

        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};

        std::mutex m_postMutex;
        std::vector<std::function<void()>> m_postQueue;

        std::mutex m_sourcesMutex;
        std::unordered_map<NativeHandle, std::function<void()>> m_sources;
    };

} // namespace vortex

namespace ms = vortex;
