# Vortex Architecture Guide

This document describes the architecture of the Vortex RunLoop library. It is
intended as a mid-level overview -- enough to understand how the pieces fit
together without reading every line of source. The companion
[WalkthroughRunLoop.md](../WalkthroughRunLoop.md) provides a line-by-line
implementation walkthrough, and the [README](../README.md) covers building and
testing.

---

## 1. What is Vortex

Vortex is a single-threaded, cross-platform event loop for C++17. Its job is
straightforward: block on a set of OS-level event sources, dispatch callbacks
when something happens, and allow other threads to safely post work into the
loop. It has no knowledge of networking protocols, serialization, or RPC -- it
is pure infrastructure.

The primary consumer of Vortex is the **Aether IPC** framework, which uses it
as an alternative to the default thread-per-client model. When a `RunLoop*` is
passed to an Aether `ServiceBase` or `ClientBase`, the framework switches from
dedicated receiver threads to single-threaded, event-driven dispatch. This
gives embedders (particularly resource-constrained systems) a way to multiplex
many connections on one thread.

Vortex is also usable standalone. Any application that needs a portable event
loop with thread-safe posting, fd/handle watching, and timers can link against
it as a library or embed it as a CMake submodule.

---

## 2. Architecture Overview

![RunLoop Architecture](diagrams/runloop-architecture.png)

The public API surface is intentionally small. A `vortex::RunLoop` instance
exposes `init()`, `run()`, `stop()`, `executeOnRunLoop()`, `addSource()` /
`removeSource()`, and `addTimer()` / `removeTimer()`. There is also a C API
(`vortex.h`) that wraps the C++ class behind opaque handles, suitable for FFI
and C-only codebases.

Underneath the public interface, three platform backends implement the actual
OS polling: **epoll** (Linux), **kqueue** (macOS/BSD), and **Win32 IOCP**
(Windows). A **stub** backend is available for platforms without native
polling or for unit testing on unsupported hosts. The backend is selected at
compile time via preprocessor guards. All backends present the same internal
interface to the rest of the class, so the core run-loop logic is written once.

The key internal data structures are a mutex-protected post queue (a
`std::vector` of callables), a mutex-protected source map (native handle to
handler callback), and a mutex-protected timer map (timer ID to entry). Two
atomic flags (`m_running` and `m_stopRequested`) coordinate lifecycle without
locking the hot path.

---

## 3. The Event Loop

![Event Dispatch Flow](diagrams/event-dispatch-flow.png)

Each iteration of `run()` follows a four-step cycle:

**Step 1 -- Drain the post queue.** The loop locks `m_postMutex`, swaps the
entire `m_postQueue` vector into a local batch (an O(1) operation), unlocks,
and then executes every callable in the batch sequentially. This swap-out
pattern ensures the mutex is held for a constant, tiny duration regardless of
how many callables have accumulated.

**Step 2 -- Block on the OS.** The loop calls the platform poll function
(`epoll_wait`, `kevent`, `GetQueuedCompletionStatusEx`, or a stub sleep). The
timeout is derived from the nearest pending timer, or infinite if no timers
exist. The loop does not busy-spin.

**Step 3 -- Dispatch ready events.** For each signaled handle, the loop checks
whether it is the internal wakeup mechanism (a pipe on POSIX, an event object
on Windows). If so, it drains the wakeup bytes and moves on. Otherwise it
looks up the handle in `m_sources` under lock, copies the handler out, and
invokes it. For source entries registered with an error callback, hangup or
error conditions (EPOLLHUP/EPOLLERR, EV_EOF) trigger the error handler and
auto-remove the source.

**Step 4 -- Check the stop flag.** If `m_stopRequested` is true, the loop
exits. Both `m_running` and `m_stopRequested` are reset on all exit paths
(normal exit, poll failure, or exception propagation), which is what makes
the loop restartable.

The reason the queue is drained _before_ blocking (rather than after) is to
ensure that callables posted between the previous poll return and the stop
check are always executed before the loop sleeps again.

---

## 4. Sources

Sources let you watch a native handle (a file descriptor on POSIX, a waitable
kernel object on Windows) for readability or signaled state. Call
`addSource(handle, handler)` from any thread. The handler fires on the loop
thread when the handle becomes ready.

If you call `addSource` with a handle that is already watched, the existing
handler is replaced atomically. On Linux, if the kernel has auto-removed the
fd (because it was closed and re-opened), the backend detects the `ENOENT`
from `EPOLL_CTL_MOD` and falls back to `EPOLL_CTL_ADD` to self-heal.

An overload accepts a second callback, `onError`, which fires when the source
encounters a hangup or error condition. After `onError` fires, the source is
automatically removed. This is useful for detecting peer disconnection on
socket file descriptors. Note that the Win32 IOCP backend cannot distinguish
data-ready from error states, so `onError` is not supported there.

`removeSource(handle)` stops watching the handle. It is safe to call even if
the handle was already removed or was never registered.

---

## 5. Timers

`addTimer(intervalMs, repeating, handler)` schedules a callback to fire after
`intervalMs` milliseconds. If `repeating` is true, the timer fires repeatedly
at that interval until explicitly cancelled. The method returns a `TimerId`
that can be passed to `removeTimer()`.

On Linux, each timer is backed by a `timerfd`, which integrates naturally with
the epoll loop. On macOS, kqueue's `EVFILT_TIMER` is used. On Windows, the
backend creates a waitable timer object. The stub backend tracks fire times
with `std::chrono::steady_clock` and computes poll timeouts accordingly.

Both `addTimer` and `removeTimer` are thread-safe. Removing a timer that has
already fired (one-shot) or been removed is a no-op. Timer handlers execute
on the loop thread, never concurrently with source handlers or posted
callables.

---

## 6. Thread-Safe Posting

`executeOnRunLoop(fn)` is the primary mechanism for sending work from an
external thread into the loop. It locks `m_postMutex`, appends the callable
to `m_postQueue`, unlocks, then writes a single byte to the wakeup pipe (or
signals the wakeup event on Windows) to unblock the poll.

The mutex protects only the vector append, not the execution. Callables run
on the loop thread with no lock held, so they have exclusive, sequential
access to whatever state the loop thread owns. FIFO ordering is guaranteed:
callables execute in the order they were posted.

This is the mechanism that Aether IPC uses to marshal responses from
connection threads back to the RunLoop-based dispatch thread.

---

## 7. Platform Backends

**epoll (Linux).** The default backend on Linux. Uses `epoll_create1` with
`EPOLL_CLOEXEC`, a `pipe2` pair for wakeup, and `timerfd_create` for timers.
Supports error/hangup detection via `EPOLLHUP` and `EPOLLERR`. No practical
limit on the number of watched file descriptors.

**kqueue (macOS/BSD).** Uses `kqueue()` with a pipe pair for wakeup and
`EVFILT_TIMER` for timers. Supports error detection via `EV_EOF`. Like epoll,
there is no practical fd limit.

**Win32 IOCP (Windows).** Uses an I/O Completion Port as the central
multiplexer. Sources are registered via `RegisterWaitForSingleObject`, which
routes waitable kernel object signals to the IOCP as completion packets.
Timers use Windows threadpool timers (`CreateThreadpoolTimer`) which also post
completions to the IOCP. The wakeup mechanism is `PostQueuedCompletionStatus`.
Dispatch uses `GetQueuedCompletionStatusEx` with a 32-entry batch to amortize
syscall overhead. IOCP has no practical limit on the number of sources or
timers, and dispatch is fair (no index-based starvation). IOCP cannot
distinguish error/hangup from normal signaling, so the `onError` callback is
not fired on Windows.

**Stub (fallback).** Compiled when no supported platform is detected. The
post queue and stop mechanisms work normally, but sources are stored without
any OS polling -- handlers are never fired. Timers use `steady_clock` to
compute sleep durations. This backend exists primarily for compilation testing
on unsupported platforms.

---

## 8. Error Handling

Vortex uses exceptions for errors that represent broken invariants or OS
failures. `init()` throws `std::logic_error` if called twice, or
`std::system_error` if the OS event mechanism cannot be created. `run()`
throws `std::system_error` if the poll syscall fails with an unexpected error
(transient interrupts via `EINTR` are silently retried). `addSource()` throws
`std::system_error` if the kernel rejects the handle registration.

Exceptions thrown by user-provided callbacks (posted callables, source
handlers, timer handlers) propagate out of `run()`. Before the exception
leaves, the loop resets `m_running` and `m_stopRequested` so that `run()` can
be called again after the caller handles the error. This makes the loop
restartable even after a crash in application code.

The C API (`vortex.h`) translates all of this into integer return codes:
`VORTEX_SUCCESS`, `VORTEX_ERR_INVALID_ARGUMENT`, `VORTEX_ERR_ALREADY_INIT`,
and `VORTEX_ERR_NOT_INIT`. Exceptions from user callbacks are caught at the C
boundary.

---

## 9. Thread Safety Model

The following operations are safe to call from **any thread** at any time:

- `executeOnRunLoop()` -- queues work for the loop thread.
- `addSource()` / `removeSource()` -- modifies the source map under lock.
- `addTimer()` / `removeTimer()` -- modifies the timer map under lock.
- `stop()` -- sets an atomic flag and wakes the loop.
- `isRunning()` -- reads an atomic flag.

The following happen exclusively on the **loop thread** (the thread that
called `run()`):

- Execution of posted callables.
- Invocation of source handlers and error callbacks.
- Invocation of timer handlers.

Because all callbacks run sequentially on the loop thread, they do not need
to synchronize with each other. If a callback modifies shared state that is
also accessed by an external thread, the external thread must use
`executeOnRunLoop()` to serialize that access, or use its own locking.

The `RunLoop` object itself must not be destroyed while `run()` is executing.
The destructor calls `stop()` as a safety net, but the caller is responsible
for joining the loop thread before letting the object go out of scope.

---

## 10. Where to Go Next

- **[WalkthroughRunLoop.md](../WalkthroughRunLoop.md)** -- A line-by-line
  walkthrough of the implementation. Covers every method, every syscall,
  every design decision with the actual code inline.

- **[README.md](../README.md)** -- Build instructions, dependency table,
  quick-start code sample, and project structure.

- **[inc/RunLoop.h](../inc/RunLoop.h)** -- The public C++ header. The
  doc-comments on each method are the canonical API reference.

- **[inc/vortex.h](../inc/vortex.h)** -- The C API header for FFI consumers.
