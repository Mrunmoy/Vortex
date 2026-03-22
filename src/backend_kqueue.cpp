#include "RunLoop.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <sys/event.h>

namespace vortex
{

    RunLoop::RunLoop() = default;

    RunLoop::~RunLoop()
    {
        if (m_running.load())
        {
            stop();
        }

        if (m_wakeupFd[0] >= 0)
        {
            close(m_wakeupFd[0]);
            close(m_wakeupFd[1]);
        }
        if (m_pollFd >= 0)
        {
            close(m_pollFd);
        }
    }

    void RunLoop::init(const char *name)
    {
        if (m_pollFd >= 0)
        {
            throw std::logic_error("RunLoop::init: already initialized");
        }

        m_name = name ? name : "";
        m_pollFd = kqueue();
        if (m_pollFd < 0)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: kqueue failed");
        }

        int fds[2];
        if (pipe(fds) != 0)
        {
            close(m_pollFd);
            m_pollFd = -1;
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: pipe failed");
        }
        m_wakeupFd[0] = fds[0];
        m_wakeupFd[1] = fds[1];

        // Set non-blocking and close-on-exec
        for (int i = 0; i < 2; ++i)
        {
            int flags = fcntl(m_wakeupFd[i], F_GETFL);
            fcntl(m_wakeupFd[i], F_SETFL, flags | O_NONBLOCK);
            fcntl(m_wakeupFd[i], F_SETFD, FD_CLOEXEC);
        }

        struct kevent ev;
        EV_SET(&ev, m_wakeupFd[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (kevent(m_pollFd, &ev, 1, nullptr, 0, nullptr) != 0)
        {
            close(m_wakeupFd[0]);
            close(m_wakeupFd[1]);
            m_wakeupFd[0] = m_wakeupFd[1] = -1;
            close(m_pollFd);
            m_pollFd = -1;
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: kevent registration failed");
        }
    }

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
            constexpr int MAX_EVENTS = 32;
            struct kevent events[MAX_EVENTS];

            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                // Execute posted callables
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

                int n = kevent(m_pollFd, nullptr, 0, events, MAX_EVENTS, nullptr);
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::system_error(errno, std::generic_category(),
                                            "RunLoop::run: kevent failed");
                }

                for (int i = 0; i < n; ++i)
                {
                    int fd = static_cast<int>(events[i].ident);
                    if (fd == m_wakeupFd[0])
                    {
                        char buf[64];
                        while (read(m_wakeupFd[0], buf, sizeof(buf)) > 0) {}
                    }
                    else
                    {
                        std::function<void()> handler;
                        {
                            std::lock_guard<std::mutex> lock(m_sourcesMutex);
                            auto it = m_sources.find(fd);
                            if (it != m_sources.end())
                            {
                                handler = it->second;
                            }
                        }
                        if (handler)
                        {
                            handler();
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

    void RunLoop::stop()
    {
        m_stopRequested.store(true, std::memory_order_release);
        wakeup();
    }

    void RunLoop::executeOnRunLoop(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(m_postMutex);
            m_postQueue.push_back(std::move(fn));
        }
        wakeup();
    }

    void RunLoop::addSource(NativeHandle fd, std::function<void()> handler)
    {
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);

        std::lock_guard<std::mutex> lock(m_sourcesMutex);

        // Save the previous handler so we can roll back on failure.
        std::function<void()> previous;
        auto it = m_sources.find(fd);
        bool alreadyWatched = (it != m_sources.end());
        if (alreadyWatched)
            previous = std::move(it->second);

        m_sources[fd] = std::move(handler);

        // kqueue EV_ADD replaces an existing filter automatically.
        if (kevent(m_pollFd, &ev, 1, nullptr, 0, nullptr) != 0)
        {
            int savedErrno = errno;
            if (alreadyWatched)
                m_sources[fd] = std::move(previous);
            else
                m_sources.erase(fd);
            throw std::system_error(savedErrno, std::generic_category(),
                                    "RunLoop::addSource: kevent failed");
        }
    }

    void RunLoop::removeSource(NativeHandle fd)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(fd);
        if (it == m_sources.end())
            return;

        std::function<void()> saved = std::move(it->second);
        m_sources.erase(it);

        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        if (kevent(m_pollFd, &ev, 1, nullptr, 0, nullptr) != 0)
        {
            // ENOENT: fd not registered (already closed/removed). Benign.
            if (errno != ENOENT)
            {
                int savedErrno = errno;
                m_sources[fd] = std::move(saved);
                throw std::system_error(savedErrno, std::generic_category(),
                                        "RunLoop::removeSource: kevent failed");
            }
        }
    }

    void RunLoop::wakeup()
    {
        char byte = 1;
        [[maybe_unused]] auto r = write(m_wakeupFd[1], &byte, 1);
    }

} // namespace vortex
