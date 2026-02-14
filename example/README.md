# Examples

Usage examples for ms-runloop.

## Building

```bash
# From the project root:
python3 build.py -e

# Or with CMake directly:
cmake -B build -DMS_RUNLOOP_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

## Running

```bash
./build/example/basic_usage
./build/example/event_notifier
```

---

## basic_usage

**File:** `basic_usage.cpp`

The simplest possible RunLoop program. It shows the four things you'll do in every
RunLoop application: create, start, post work, and stop.

### What it does

```
Hello from the run loop thread!
  task 0
  task 1
  task 2
  task 3
  task 4
Stopping...
Done.
```

### How it works

**1. Create and initialize**

```cpp
ms::RunLoop loop;
loop.init("Example");
```

`init()` creates the epoll instance and the internal wakeup pipe. The string name
is for debugging — it doesn't affect behavior.

**2. Start the loop on a background thread**

```cpp
std::thread t([&] { loop.run(); });
```

`run()` blocks the calling thread and turns it into the loop thread. All callables
posted via `executeOnRunLoop()` will execute on this thread. You always need to run
the loop on its own thread unless you want to block `main()`.

**3. Post work from any thread**

```cpp
loop.executeOnRunLoop([&] {
    std::printf("Hello from the run loop thread!\n");
});
```

`executeOnRunLoop()` is thread-safe — you can call it from `main()`, from other
worker threads, or even from within another posted callable. The callable is queued
and will execute on the loop thread in FIFO order.

**4. Post multiple items — they execute in order**

```cpp
for (int i = 0; i < 5; ++i) {
    loop.executeOnRunLoop([i] {
        std::printf("  task %d\n", i);
    });
}
```

Callables are guaranteed to execute in the order they were posted. This is a key
property — you can reason about sequencing without locks.

**5. Stop from within a callable**

```cpp
loop.executeOnRunLoop([&] {
    std::printf("Stopping...\n");
    loop.stop();
});
```

By posting `stop()` as a callable, you guarantee all previously posted work
completes before the loop exits. Calling `stop()` directly from another thread
would interrupt the loop immediately (after the current batch finishes).

**6. Join the thread**

```cpp
t.join();
```

After `stop()`, `run()` returns, and the thread can be joined.

### When to use this pattern

This is the right pattern when you have a single thread that needs to process
work items posted by other threads — UI event loops, message dispatchers,
task schedulers, or any situation where you want to serialize access to
shared state without explicit locking.

---

## event_notifier

**File:** `event_notifier.cpp`

A realistic example showing how to use RunLoop as a thread-safe event notification
bus between decoupled components. Three classes interact without knowing about
each other's internals, and none of them need mutexes.

### What it does

```
[Logger] temperature = 22.5
[Logger] pressure = 1013.0
[Alert]  pressure exceeded threshold (1013.0 > 80.0)
[Logger] temperature = 85.3
[Alert]  temperature exceeded threshold (85.3 > 80.0)
[Logger] humidity = 45.0
[Logger] pressure = 1050.7
[Alert]  pressure exceeded threshold (1050.7 > 80.0)
Done.
```

### The components

**`SensorEvent`** — a simple data struct passed between components:

```cpp
struct SensorEvent {
    std::string sensorName;
    double value;
};
```

**`Logger`** — receives events and logs them. Has no locks, no thread awareness.
It just prints:

```cpp
class Logger {
public:
    void onSensorEvent(const SensorEvent &event) {
        std::printf("[Logger] %s = %.1f\n", event.sensorName.c_str(), event.value);
    }
};
```

**`AlertManager`** — receives events and checks against a threshold. Also has
no locks — it just reads its own member variable:

```cpp
class AlertManager {
public:
    explicit AlertManager(double threshold) : m_threshold(threshold) {}

    void onSensorEvent(const SensorEvent &event) {
        if (event.value > m_threshold) {
            std::printf("[Alert]  %s exceeded threshold ...\n", ...);
        }
    }
};
```

**`SensorMonitor`** — the producer. It holds a reference to the RunLoop and a
list of listener callbacks. When a sensor reading arrives, it posts a notification
to the run loop thread:

```cpp
class SensorMonitor {
public:
    using Callback = std::function<void(const SensorEvent &)>;

    SensorMonitor(ms::RunLoop &loop) : m_loop(loop) {}

    // Thread-safe: mutation happens on the loop thread
    void addListener(Callback cb) {
        m_loop.executeOnRunLoop([this, cb = std::move(cb)]() mutable {
            m_listeners.push_back(std::move(cb));
        });
    }

    void simulateReadings() {
        // For each reading:
        m_loop.executeOnRunLoop([this, event] { notify(event); });
    }
};
```

Note that `addListener()` routes through `executeOnRunLoop()` so that both
listener registration and notification happen on the same thread. This means
listeners can safely be added at any time — even after the loop is running —
without data races on `m_listeners`.

### How it works

**1. Wire up components**

```cpp
ms::RunLoop loop;
loop.init("EventBus");

Logger logger;
AlertManager alerts(80.0);

SensorMonitor monitor(loop);
monitor.addListener([&](const SensorEvent &e) { logger.onSensorEvent(e); });
monitor.addListener([&](const SensorEvent &e) { alerts.onSensorEvent(e); });
```

The `SensorMonitor` doesn't know about `Logger` or `AlertManager`. It just calls
its registered callbacks. The wiring happens in `main()`.

**2. Producer posts events from the main thread**

```cpp
monitor.simulateReadings();
```

Inside `simulateReadings()`, each sensor reading is wrapped in an `executeOnRunLoop()`
call. This means the actual notification happens on the loop thread, not on the
calling thread.

**3. Handlers execute sequentially on the loop thread**

When the loop thread processes a posted callable, it calls `notify()`, which
iterates through listeners and invokes each callback. Because this all happens
on a single thread:

- `Logger::onSensorEvent()` and `AlertManager::onSensorEvent()` never run
  concurrently with each other
- Neither handler needs a mutex to protect its internal state
- Event ordering is preserved — events arrive in the order they were posted

**4. Clean shutdown**

```cpp
// Inside simulateReadings():
m_loop.executeOnRunLoop([this] { m_loop.stop(); });
```

The stop is posted as the last callable, so all sensor events are delivered
before the loop exits.

### When to use this pattern

This pattern is ideal when you have:

- **Multiple producers** posting events from different threads (sensor drivers,
  network callbacks, timer ticks)
- **Multiple consumers** that need to process events without races (loggers,
  state machines, UI updaters)
- **No desire to manage locks** in your handler code — the RunLoop serializes
  everything for you

Real-world uses: GUI event loops, game engine message dispatchers, audio
processing pipelines, IoT sensor aggregation, microservice event buses.

### Key insight

The RunLoop turns a multi-threaded problem into a single-threaded one. Producers
can fire events from any thread, but handlers always run sequentially on the
loop thread. This eliminates an entire class of concurrency bugs — data races,
deadlocks, and lock-ordering issues — by design rather than by discipline.
