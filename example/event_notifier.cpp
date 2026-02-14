// Demonstrates using RunLoop as an event notification bus between components.
//
// A SensorMonitor detects events and notifies a Logger and an AlertManager,
// all running on the same RunLoop thread. This ensures the handlers never
// race with each other — no locks needed in the receivers.

#include "RunLoop.h"

#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// ── Event types ─────────────────────────────────────────────────────

struct SensorEvent
{
    std::string sensorName;
    double value;
};

// ── Logger: receives events on the run loop thread ──────────────────

class Logger
{
public:
    void onSensorEvent(const SensorEvent &event)
    {
        std::printf("[Logger] %s = %.1f\n", event.sensorName.c_str(), event.value);
    }
};

// ── AlertManager: checks thresholds on the run loop thread ──────────

class AlertManager
{
public:
    explicit AlertManager(double threshold)
        : m_threshold(threshold) {}

    void onSensorEvent(const SensorEvent &event)
    {
        if (event.value > m_threshold)
        {
            std::printf("[Alert]  %s exceeded threshold (%.1f > %.1f)\n",
                        event.sensorName.c_str(), event.value, m_threshold);
        }
    }

private:
    double m_threshold;
};

// ── SensorMonitor: produces events from a worker thread ─────────────

class SensorMonitor
{
public:
    using Callback = std::function<void(const SensorEvent &)>;

    SensorMonitor(ms::RunLoop &loop)
        : m_loop(loop) {}

    // Thread-safe: the actual mutation happens on the loop thread,
    // so it never races with notify().
    void addListener(Callback cb)
    {
        m_loop.executeOnRunLoop([this, cb = std::move(cb)]() mutable
                                { m_listeners.push_back(std::move(cb)); });
    }

    // Simulate sensor readings from a background thread
    void simulateReadings()
    {
        struct Reading
        {
            const char *name;
            double value;
        };

        Reading readings[] = {
            {"temperature", 22.5},
            {"pressure", 1013.0},
            {"temperature", 85.3}, // will trigger alert
            {"humidity", 45.0},
            {"pressure", 1050.7}, // will trigger alert
        };

        for (const auto &r : readings)
        {
            SensorEvent event{r.name, r.value};

            // Post notification to the run loop thread
            m_loop.executeOnRunLoop([this, event]
                                   { notify(event); });
        }

        // Stop the loop after all events are delivered
        m_loop.executeOnRunLoop([this]
                                { m_loop.stop(); });
    }

private:
    void notify(const SensorEvent &event)
    {
        for (auto &cb : m_listeners)
        {
            cb(event);
        }
    }

    ms::RunLoop &m_loop;
    std::vector<Callback> m_listeners;
};

// ── main ────────────────────────────────────────────────────────────

int main()
{
    ms::RunLoop loop;
    loop.init("EventBus");

    Logger logger;
    AlertManager alerts(80.0);

    SensorMonitor monitor(loop);
    monitor.addListener([&](const SensorEvent &e)
                        { logger.onSensorEvent(e); });
    monitor.addListener([&](const SensorEvent &e)
                        { alerts.onSensorEvent(e); });

    // Run the loop on a background thread
    std::thread loopThread([&]
                           { loop.run(); });

    // Simulate sensor readings from the main thread
    monitor.simulateReadings();

    loopThread.join();

    std::printf("Done.\n");
    return 0;
}
