#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>
#include "cotamer/timer_heap.hh"
#include "cotamer/event_handle.hh"

// cotamer.hh
//    Public interface to the Cotamer coroutine library.

// Define COTAMER_STATS to 1 to collect statistics.
// #define COTAMER_STATS 1

namespace cotamer {

// event
//    A one-shot signal. Starts untriggered; once triggered, it stays triggered
//    forever. Coroutines suspend on events with `co_await`. Events are
//    reference-counted and cheaply copyable: copies share the same underlying
//    signal.

class event {
public:
    inline event();
    inline event(detail::event_handle ev);
    explicit inline event(nullptr_t);
    ~event() = default;
    event(const event&) = default;
    event(event&&) = default;
    event& operator=(const event&) = default;
    event& operator=(event&&) = default;

    inline bool triggered() const noexcept;    // has triggered
    inline bool idle() const noexcept;         // has no listeners
    inline bool empty() const noexcept;        // can be garbage collected

    inline bool trigger();
    inline event& arm();

    inline const detail::event_handle& handle() const& noexcept;
    inline detail::event_handle&& handle() && noexcept;
    std::string debug_info() const;

private:
    detail::event_handle ep_;
};


// task<T>
//    A coroutine that produces a value of type T (or void). Tasks start
//    running eagerly when called. To retrieve the result, `co_await` the task;
//    this suspends the caller until the task completes. A task can also be
//    detached (with `detach()`), in which case it runs to completion
//    independently and its result is discarded.
//
//    Tasks support lazy execution via `interest{}`. A task that `co_await`s
//    `interest{}` suspends until someone expresses interest in its result
//    (by `co_await`ing the task or calling `start()`).

template <typename T = void>
class task {
public:
    using promise_type = detail::task_promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    inline task() noexcept = default;
    explicit inline task(handle_type handle) noexcept;
    inline task(task&& x) noexcept;
    inline task& operator=(task&& x) noexcept;
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    inline ~task();

    inline void start();            // start a task waiting for interest{}
    inline void detach();           // coroutine survives task<> deletion
    inline bool done();             // has coroutine completed?
    inline event completion();      // event that triggers on done()

    detail::task_awaiter<T> operator co_await() const noexcept;

private:
    friend struct detail::task_promise<T>;
    handle_type handle_;
};


// Sentinel types used with `co_await`:
// co_await interest{} — suspend until someone awaits this task
// co_await interest_event{} — obtain the interest event without suspending
struct interest {};
struct interest_event {};

// Event combinators.
// any(e1, e2, ...) — triggers when any one of its arguments triggers.
// all(e1, e2, ...) — triggers when all of its arguments have triggered.
// Arguments can be events, awaitables (converted to events via helper
// coroutines), or `interest{}` (a lazy-start placeholder).
template <typename... Es> inline event any(Es&&... es);
template <typename... Es> inline event all(Es&&... es);

// attempt(task, events...) — race a task against events. Returns the task's
// result (wrapped in optional) if the task completes first, or nullopt if
// one of the events triggers first.
template <typename T, typename... Es>
[[nodiscard]] task<std::optional<T>> attempt(task<T> t, Es&&... es);
template <typename T, typename... Es>
[[nodiscard]] task<std::optional<T>> attempt(task<std::optional<T>> t, Es&&... es);
template <typename... Es>
[[nodiscard]] task<std::optional<std::monostate>> attempt(task<void> t, Es&&... es);


// driver
//    The event loop. Maintains a queue of ready coroutines, a queue of
//    “asap” events (triggered before the next time step), and a timer heap.
//    Time is simulated: the clock advances by one tick per coroutine
//    resumption, and jumps forward to the next timer when idle.
//
//    Each thread has its own driver stored in `driver::current`. The free
//    functions `now()`, `after()`, `loop()`, etc. delegate to it.

using system_time_point = std::chrono::system_clock::time_point;
using steady_time_point = std::chrono::steady_clock::time_point;
using duration = std::chrono::steady_clock::duration;

class driver {
public:
    driver();
    ~driver();
    driver(const driver&) = delete;
    driver(driver&&) = delete;
    driver& operator=(const driver&) = delete;
    driver& operator=(driver&&) = delete;

    inline system_time_point now() noexcept;        // current system time (might go backwards)
    inline steady_time_point steady_now() noexcept; // time since boot (monotonic)
    inline void step_time() noexcept;

    inline void keepalive(event);

    inline void asap(event);
    inline event asap();

    inline void at(steady_time_point t, event);
    inline event at(steady_time_point t);
    inline void at(system_time_point t, event);
    inline event at(system_time_point t);
    inline void after(duration d, event);
    inline event after(duration d);
    template <typename Rep, typename Period>
    inline void after(const std::chrono::duration<Rep, Period>&, event);
    template <typename Rep, typename Period>
    inline event after(const std::chrono::duration<Rep, Period>&);

    inline void loop();
    inline void poll();
    void clear();
    bool clearing() const { return clearing_; }

    // introspection
    inline size_t timer_size() const;

    static thread_local std::unique_ptr<driver> current;

private:
    friend struct detail::event_body;
    friend class driver_guard;
    template <typename T> friend struct detail::task_final_awaiter;

    system_time_point virtual_epoch_;
    steady_time_point snow_;
    bool clearing_ = false;
    int guard_count_ = 0;
    std::deque<detail::event_handle> asap_;
    timer_heap<detail::event_handle> timed_;
    std::vector<detail::event_handle> keepalives_;

    static constexpr uint32_t df_lock = 1;
    static constexpr uint32_t df_nonempty = 2;
    std::atomic<uint32_t> lock_ = 0;
    std::vector<detail::event_handle> migrate_;

    static constexpr size_t asap_quota = 0x1000; // run at most 4096 ASAP tasks per poll()

    inline uint32_t lock();
    inline void unlock(uint32_t flags);
    void migrate_asap(detail::event_handle eh);
    inline void migrate_wake();
    void finish_migrate();

    enum class looptype { complete, poll };
    void loop(looptype);
};


// Time and scheduling functions (operate on driver::current)

inline void loop();                    // run event loop until quiescent
inline void poll();                    // run event loop once without blocking
inline void clear();                   // cancel all pending events
void reset();                          // destroy and recreate driver

inline system_time_point now() noexcept;
inline steady_time_point steady_now() noexcept;
inline void step_time() noexcept;

inline void keepalive(event);          // loop continues until event triggers

inline event asap();                   // triggers before next time step

inline event after(duration);          // triggers after a delay
template <typename Rep, typename Period>
inline event after(const std::chrono::duration<Rep, Period>&);
inline event at(steady_time_point);    // triggers at an absolute time
inline event at(system_time_point);    // triggers at an absolute system time


// driver_guard
//    Keeps the driver loop alive as long as it exists. Use when waiting
//    for an event invisible to the driver — e.g., an event triggered on
//    a different thread.

class driver_guard {
public:
    inline driver_guard();
    inline driver_guard(driver_guard&&);
    inline driver_guard& operator=(driver_guard&&);
    driver_guard(const driver_guard&) = delete;
    driver_guard& operator=(const driver_guard&) = delete;
    inline ~driver_guard();

private:
    driver* drv_;
};


// Error codes and exception type.

enum class cotamer_errc {
    cross_driver_await = 1
};

struct cotamer_error : std::logic_error {
    explicit cotamer_error(cotamer_errc ec);
    inline cotamer_errc code() const noexcept { return errc_; }

private:
    cotamer_errc errc_;
    static constexpr const char* message(cotamer_errc ec) noexcept;
};


// Statistics.

#if COTAMER_STATS
struct statistics {
    std::atomic<size_t> promises_allocated;
    std::atomic<size_t> promises_destroyed;
    std::atomic<size_t> events_allocated;
    std::atomic<size_t> events_destroyed;
};
extern statistics stats;
#endif

}

#include "cotamer/cotamer_impl.hh"
