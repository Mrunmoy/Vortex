#include "vortex.h"
#include "RunLoop.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>

struct vortex_impl
{
    vortex::RunLoop loop;
    std::mutex runMutex;
    std::condition_variable runCv;
    int activeRuns = 0;
    std::atomic<bool> initialized{false};
};

namespace
{

    int mapInitError()
    {
        try
        {
            throw;
        }
        catch (const std::logic_error &)
        {
            return VORTEX_ERR_ALREADY_INIT;
        }
        catch (...)
        {
            return VORTEX_ERR_INVALID_ARGUMENT;
        }
    }

    int mapRuntimeError()
    {
        try
        {
            throw;
        }
        catch (const std::logic_error &)
        {
            return VORTEX_ERR_NOT_INIT;
        }
        catch (const std::system_error &)
        {
            return VORTEX_ERR_NOT_INIT;
        }
        catch (...)
        {
            return VORTEX_ERR_INVALID_ARGUMENT;
        }
    }

} // namespace

extern "C"
{

    int vortex_create(vortex_t *out)
    {
        if (!out)
            return VORTEX_ERR_INVALID_ARGUMENT;

        try
        {
            *out = new vortex_impl();
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return VORTEX_ERR_INVALID_ARGUMENT;
        }
    }

    int vortex_init(vortex_t loop, const char *name)
    {
        if (!loop || !name)
            return VORTEX_ERR_INVALID_ARGUMENT;

        try
        {
            loop->loop.init(name);
            loop->initialized = true;
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return mapInitError();
        }
    }

    int vortex_run(vortex_t loop)
    {
        if (!loop)
            return VORTEX_ERR_INVALID_ARGUMENT;
        if (!loop->initialized)
            return VORTEX_ERR_NOT_INIT;

        {
            std::lock_guard<std::mutex> lock(loop->runMutex);
            ++loop->activeRuns;
        }

        try
        {
            loop->loop.run();
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(loop->runMutex);
                --loop->activeRuns;
            }
            loop->runCv.notify_all();
            return mapRuntimeError();
        }

        {
            std::lock_guard<std::mutex> lock(loop->runMutex);
            --loop->activeRuns;
        }
        loop->runCv.notify_all();
        return VORTEX_SUCCESS;
    }

    void vortex_stop(vortex_t loop)
    {
        if (loop)
            loop->loop.stop();
    }

    void vortex_destroy(vortex_t loop)
    {
        if (!loop)
            return;

        loop->loop.stop();
        std::unique_lock<std::mutex> lock(loop->runMutex);
        loop->runCv.wait(lock, [loop] { return loop->activeRuns == 0; });
        lock.unlock();
        delete loop;
    }

    int vortex_post(vortex_t loop, vortex_callable_cb cb, void *user_data)
    {
        if (!loop || !cb)
            return VORTEX_ERR_INVALID_ARGUMENT;
        if (!loop->initialized)
            return VORTEX_ERR_NOT_INIT;

        try
        {
            loop->loop.executeOnRunLoop([cb, user_data]() { cb(user_data); });
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return mapRuntimeError();
        }
    }

    int vortex_add_source(vortex_t loop, int fd, vortex_source_cb cb, void *user_data)
    {
        if (!loop || !cb || fd < 0)
            return VORTEX_ERR_INVALID_ARGUMENT;
        if (!loop->initialized)
            return VORTEX_ERR_NOT_INIT;

        try
        {
            loop->loop.addSource(fd, [cb, user_data]() { cb(user_data); });
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return mapRuntimeError();
        }
    }

    void vortex_remove_source(vortex_t loop, int fd)
    {
        if (loop && fd >= 0)
            loop->loop.removeSource(fd);
    }

    int vortex_add_source_with_error(vortex_t loop, int fd,
                                     vortex_source_cb cb, void *user_data,
                                     vortex_source_cb on_error, void *error_data)
    {
        if (!loop || !cb || fd < 0)
            return VORTEX_ERR_INVALID_ARGUMENT;
        if (!loop->initialized)
            return VORTEX_ERR_NOT_INIT;

        std::function<void()> errorFn;
        if (on_error)
            errorFn = [on_error, error_data]() { on_error(error_data); };

        try
        {
            loop->loop.addSource(
                fd,
                [cb, user_data]() { cb(user_data); },
                std::move(errorFn));
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return mapRuntimeError();
        }
    }

    int vortex_add_timer(vortex_t loop, uint32_t interval_ms, int repeating,
                         vortex_timer_cb cb, void *user_data, uint64_t *out_id)
    {
        if (!loop || !cb || !out_id)
            return VORTEX_ERR_INVALID_ARGUMENT;
        if (!loop->initialized)
            return VORTEX_ERR_NOT_INIT;

        try
        {
            auto id = loop->loop.addTimer(interval_ms, repeating != 0,
                                          [cb, user_data]() { cb(user_data); });
            *out_id = id;
            return VORTEX_SUCCESS;
        }
        catch (...)
        {
            return mapRuntimeError();
        }
    }

    void vortex_remove_timer(vortex_t loop, uint64_t timer_id)
    {
        if (loop)
            loop->loop.removeTimer(timer_id);
    }

    int vortex_is_running(vortex_t loop)
    {
        return (loop && loop->loop.isRunning()) ? 1 : 0;
    }

    const char *vortex_name(vortex_t loop)
    {
        return loop ? loop->loop.name() : nullptr;
    }

    void vortex_free(void *ptr)
    {
        std::free(ptr);
    }

} // extern "C"
