# Walkthrough

This document is a guided walkthrough of the **exact** implementation in `inc/RunLoop.h` and `src/RunLoop.cpp`.
It is intentionally code-driven: each step points at the real lines you ship.

If you only want to _use_ the library, go back to the main README.

---

## 0) What we are building

You describe the goals right at the top of the header:

```cpp
// Pure event loop. Runs on a dedicated thread, allows other
// components to post work to that thread. No transport knowledge.
//
// Usage:
//   RunLoop loop;
//   loop.init("MyApp");
//   loop.executeOnRunLoop([&] { /* runs on loop thread */ });
//   loop.run();  // blocks until stop()
```

Key constraints (we'll justify them as we go):
- **Single loop thread** — `run()` blocks the calling thread and becomes the loop thread
- **Thread-safe posting** — `executeOnRunLoop()` can be called from any thread
- **No transport knowledge** — the loop knows nothing about sockets, RPC, or protocols

---

## 1) Versioning inside the class

You keep a single source of truth for the library version, nested inside the class to avoid
conflicts when used alongside other `ms::` libraries:

```cpp
struct Version
{
    static constexpr uint8_t major = 1;
    static constexpr uint8_t minor = 0;
    static constexpr uint8_t patch = 0;
    static constexpr uint32_t packed = (major << 16) | (minor << 8) | patch;
};
```

Access it as `ms::RunLoop::Version::major` or check the packed value for compatibility.

---

## 2) The public API surface

The class is intentionally small — five methods and two queries:

```cpp
class RunLoop
{
public:
    void init(const char *name);
    void run();
    void stop();
    void executeOnRunLoop(std::function<void()> fn);

    bool isRunning() const;
    const char *name() const;
};
```

Copy and move are deleted. You create one, you own one.

```cpp
RunLoop(const RunLoop &) = delete;
RunLoop &operator=(const RunLoop &) = delete;
```

---

## 3) Private state: the pieces that make it work

```cpp
const char *m_name = "";
int m_epollFd = -1;
int m_wakeupFd[2] = {-1, -1};

std::atomic<bool> m_running{false};
std::atomic<bool> m_stopRequested{false};

std::mutex m_postMutex;
std::vector<std::function<void()>> m_postQueue;
```

What each piece does:
- `m_name` — debug identifier, set by `init()`
- `m_epollFd` — the epoll instance that drives the event loop
- `m_wakeupFd` — a pipe pair used to wake epoll from another thread
- `m_running` / `m_stopRequested` — atomic flags for lifecycle coordination
- `m_postMutex` + `m_postQueue` — the thread-safe work queue

---

## 4) `init()`: setting up the plumbing

```cpp
void RunLoop::init(const char *name)
{
    m_name = name;
    m_epollFd = epoll_create1(EPOLL_CLOEXEC);

    if (pipe2(m_wakeupFd, O_CLOEXEC | O_NONBLOCK) == 0)
    {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = m_wakeupFd[0];
        epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd[0], &ev);
    }
}
```

### The epoll instance

`epoll_create1(EPOLL_CLOEXEC)` creates a new epoll file descriptor.
`EPOLL_CLOEXEC` means it won't leak into child processes if you ever `fork()+exec()`.

### The wakeup pipe

`pipe2()` creates a pair of file descriptors: `m_wakeupFd[0]` for reading, `m_wakeupFd[1]` for writing.

Flags:
- `O_CLOEXEC` — don't leak on exec
- `O_NONBLOCK` — reads never block (important: the loop drains all pending bytes)

The read end is registered with epoll. When anyone writes a byte into `m_wakeupFd[1]`,
epoll wakes up and the loop can process posted work.

### Why a pipe and not eventfd?

A pipe is universally available on all POSIX systems. `eventfd` is Linux-specific.
For this simple use case (single-byte wakeup signal), a pipe is sufficient and portable.

---

## 5) `run()`: the event loop

This is the heart of the library:

```cpp
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
                while (read(m_wakeupFd[0], buf, sizeof(buf)) > 0)
                {
                }
            }
        }
    }

    m_running.store(false, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
}
```

### Step by step

1. **Set running flag** — `release` so other threads see it after `init()` completes.

2. **Drain the work queue** — Lock the mutex, swap the entire vector out (O(1)),
   unlock, then execute everything. The swap-out pattern means the mutex is held
   for a tiny constant time regardless of how many callables are queued.

3. **Wait for events** — `epoll_wait(..., -1)` blocks until something happens.
   The `-1` timeout means "wait forever" — no busy-spinning, no polling.

4. **Drain the wakeup pipe** — When the pipe is readable, we consume all bytes.
   Multiple `executeOnRunLoop()` calls may have written multiple bytes — we drain
   them all so the pipe doesn't fill up.

5. **Check stop** — If `m_stopRequested` is true, exit the loop.

6. **Clean up** — Reset both flags so the loop can be restarted.

### Why drain before wait?

Callables may have been posted between the last `epoll_wait` return and the stop check.
By draining the queue at the top of each iteration, we guarantee all posted work
is executed before we block again.

### Memory ordering

- `m_running`: `release` store pairs with `acquire` load in `isRunning()`
- `m_stopRequested`: `acquire` load pairs with `release` store in `stop()`

This ensures that when `isRunning()` returns `true`, the loop is actually dispatching.
And when `stop()` sets the flag, the loop sees it on its next iteration.

---

## 6) `stop()`: signalling shutdown

```cpp
void RunLoop::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    wakeup();
}
```

Two things happen:
1. Set the flag (so the loop's `while` condition becomes false)
2. Wake up epoll (so the loop isn't stuck in `epoll_wait` forever)

`stop()` is thread-safe. You can call it from:
- Another thread
- From inside a posted callable (the loop finishes the current batch, then exits)
- Before `run()` is called (the loop will exit immediately when started)

---

## 7) `executeOnRunLoop()`: posting work

```cpp
void RunLoop::executeOnRunLoop(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(m_postMutex);
        m_postQueue.push_back(std::move(fn));
    }
    wakeup();
}
```

The pattern:
1. Lock the mutex, append the callable, unlock
2. Write a byte to the wakeup pipe

The mutex protects the vector, not the execution. Callables execute
on the loop thread without any lock held — they have exclusive access
to the loop thread.

---

## 8) `wakeup()`: the one-byte signal

```cpp
void RunLoop::wakeup()
{
    char byte = 1;
    [[maybe_unused]] auto r = write(m_wakeupFd[1], &byte, 1);
}
```

This writes a single byte to the write end of the pipe. The value doesn't matter —
epoll watches for "readable", not for specific data.

`[[maybe_unused]]` silences the compiler warning about the unused return value.
In practice, this write can only fail if the pipe buffer is full (64KB on Linux),
which would mean tens of thousands of unprocessed wakeups — a situation that
would indicate a much bigger problem.

---

## 9) Destructor: cleanup

```cpp
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
```

Safety:
- If the loop is still running, stop it first
- Close file descriptors only if they were successfully opened
- The `-1` sentinel values (set in the header) prevent double-close

---

## 10) The threading story in one sentence

- Any thread can call `executeOnRunLoop()` to queue work (mutex-protected).
- The loop thread drains the queue and executes callables sequentially (no lock held).
- `stop()` sets an atomic flag and wakes epoll; the loop exits on its next iteration.

---

## 11) Why this design?

### Why epoll and not just a condition variable?

Epoll gives you a future-proof event loop. Right now, the only event source is the
wakeup pipe. But the same `m_epollFd` can monitor sockets, timers, signalfd, eventfd —
anything with a file descriptor. The loop is ready to grow without changing its core structure.

### Why `std::function<void()>` and not a custom callable?

`std::function` is the standard vocabulary type. It captures lambdas, binds,
function pointers — anything callable. The small-buffer optimization in most
standard libraries means small lambdas don't allocate.

### Why swap instead of pop?

Swapping the entire vector out of the mutex is O(1) regardless of queue size.
The alternative — popping one at a time — would hold the mutex during execution
and block other threads from posting.

---
