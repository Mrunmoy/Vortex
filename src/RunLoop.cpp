#include "RunLoop.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <sys/epoll.h>

namespace ms
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
        if (m_epollFd >= 0)
        {
            close(m_epollFd);
        }
    }

    void RunLoop::init(const char *name)
    {
        m_name = name ? name : "";
        m_epollFd = epoll_create1(EPOLL_CLOEXEC);
        if (m_epollFd < 0)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: epoll_create1 failed");
        }

        if (pipe2(m_wakeupFd, O_CLOEXEC | O_NONBLOCK) != 0)
        {
            close(m_epollFd);
            m_epollFd = -1;
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: pipe2 failed");
        }

        struct epoll_event ev
        {
        };
        ev.events = EPOLLIN;
        ev.data.fd = m_wakeupFd[0];
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd[0], &ev) != 0)
        {
            close(m_wakeupFd[0]);
            close(m_wakeupFd[1]);
            m_wakeupFd[0] = m_wakeupFd[1] = -1;
            close(m_epollFd);
            m_epollFd = -1;
            throw std::system_error(errno, std::generic_category(),
                                    "RunLoop::init: epoll_ctl failed");
        }
    }

    void RunLoop::run()
    {
        m_running.store(true, std::memory_order_release);

        // Ensure flags are reset on all exit paths (normal exit, exceptions
        // from handlers, or epoll_wait failures).
        auto resetFlags = [this]()
        {
            m_running.store(false, std::memory_order_release);
            m_stopRequested.store(false, std::memory_order_release);
        };

        try
        {
            constexpr int MAX_EVENTS = 32;
            struct epoll_event events[MAX_EVENTS];

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

                int n = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::system_error(errno, std::generic_category(),
                                            "RunLoop::run: epoll_wait failed");
                }

                for (int i = 0; i < n; ++i)
                {
                    if (events[i].data.fd == m_wakeupFd[0])
                    {
                        char buf[64];
                        while (read(m_wakeupFd[0], buf, sizeof(buf)) > 0) {}
                    }
                    else
                    {
                        std::function<void()> handler;
                        {
                            std::lock_guard<std::mutex> lock(m_sourcesMutex);
                            auto it = m_sources.find(events[i].data.fd);
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

    void RunLoop::addSource(int fd, std::function<void()> handler)
    {
        struct epoll_event ev
        {
        };
        ev.events = EPOLLIN;
        ev.data.fd = fd;

        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(fd);
        bool alreadyWatched = (it != m_sources.end());

        // Save the previous handler so we can roll back on failure.
        std::function<void()> previous;
        if (alreadyWatched)
            previous = std::move(it->second);

        m_sources[fd] = std::move(handler);
        int op = alreadyWatched ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (epoll_ctl(m_epollFd, op, fd, &ev) != 0)
        {
            // If the fd was believed to be watched but epoll reports ENOENT,
            // it was likely auto-removed (e.g. after being closed). In that
            // case, retry with EPOLL_CTL_ADD to resynchronize our state.
            if (alreadyWatched && errno == ENOENT)
            {
                op = EPOLL_CTL_ADD;
                if (epoll_ctl(m_epollFd, op, fd, &ev) == 0)
                {
                    return;
                }
            }

            int savedErrno = errno;
            if (alreadyWatched)
                m_sources[fd] = std::move(previous);
            else
                m_sources.erase(fd);
            throw std::system_error(savedErrno, std::generic_category(),
                                    "RunLoop::addSource: epoll_ctl failed");
        }
    }

    void RunLoop::removeSource(int fd)
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        auto it = m_sources.find(fd);
        if (it == m_sources.end())
            return;

        // Save the handler so we can roll back if epoll_ctl fails unexpectedly.
        std::function<void()> saved = std::move(it->second);
        m_sources.erase(it);

        if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) != 0)
        {
            // ENOENT: fd was not registered in epoll (e.g., it was already
            // closed and auto-removed by the kernel). This is benign — the
            // descriptor is no longer monitored either way.
            if (errno != ENOENT)
            {
                int savedErrno = errno;
                m_sources[fd] = std::move(saved);
                throw std::system_error(savedErrno, std::generic_category(),
                                        "RunLoop::removeSource: epoll_ctl failed");
            }
        }
    }

    void RunLoop::wakeup()
    {
        char byte = 1;
        [[maybe_unused]] auto r = write(m_wakeupFd[1], &byte, 1);
    }

} // namespace ms
