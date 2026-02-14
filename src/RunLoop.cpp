#include "RunLoop.h"

#include <fcntl.h>
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
        m_name = name;
        m_epollFd = epoll_create1(EPOLL_CLOEXEC);

        if (pipe2(m_wakeupFd, O_CLOEXEC | O_NONBLOCK) == 0)
        {
            struct epoll_event ev
            {
            };
            ev.events = EPOLLIN;
            ev.data.fd = m_wakeupFd[0];
            epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd[0], &ev);
        }
    }

    void RunLoop::run()
    {
        m_running.store(true, std::memory_order_release);

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

        m_running.store(false, std::memory_order_release);
        m_stopRequested.store(false, std::memory_order_release);
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
        {
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            m_sources[fd] = std::move(handler);
        }

        struct epoll_event ev
        {
        };
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev);
    }

    void RunLoop::removeSource(int fd)
    {
        epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);

        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources.erase(fd);
    }

    void RunLoop::wakeup()
    {
        char byte = 1;
        [[maybe_unused]] auto r = write(m_wakeupFd[1], &byte, 1);
    }

} // namespace ms
