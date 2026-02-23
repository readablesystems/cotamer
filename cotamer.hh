#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>
#include "detail/timer_heap.hh"
#include "detail/event_handle.hh"

// cotamer.hh
//    Public interface to the Cotamer coroutine library.

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

    inline void trigger();
    inline bool triggered() const noexcept;

    inline bool empty() const noexcept;
    inline bool idle() const noexcept;

    inline const detail::event_handle& handle() const&;
    inline detail::event_handle&& handle() &&;
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

    explicit inline task(handle_type handle) noexcept;
    inline task(task&& x) noexcept;
    inline task& operator=(task&& x) noexcept;
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    inline ~task();

    inline void detach();

    inline event completion();
    inline bool done();

    inline void start();

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

// Exception thrown during driver::clear() to unwind suspended coroutines.
struct clearing_error {};


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
task<std::optional<T>> attempt(task<T> t, Es&&... es);
template <typename T, typename... Es>
task<std::optional<T>> attempt(task<std::optional<T>> t, Es&&... es);
template <typename... Es>
task<std::optional<bool>> attempt(task<void> t, Es&&... es);


// Time and scheduling functions (operate on driver::main).
using clock = std::chrono::system_clock;
inline clock::time_point now();
inline void step_time();

inline event asap();                   // triggers before next time step
inline event after(clock::duration);   // triggers after a delay
inline event at(clock::time_point);    // triggers at an absolute time

inline void loop();                    // run event loop until quiescent
inline void clear();                   // cancel all pending events
void reset();                          // destroy and recreate driver


// driver
//    The event loop. Maintains a queue of ready coroutines, a queue of
//    “asap” events (triggered before the next time step), and a timer heap.
//    Time is simulated: the clock advances by one tick per coroutine
//    resumption, and jumps forward to the next timer when idle.
//
//    A single global driver is stored in `driver::main`. The free functions
//    `now()`, `after()`, `loop()`, etc. delegate to it.

class driver {
public:
    driver();
    ~driver();
    driver(const driver&) = delete;
    driver(driver&&) = delete;
    driver& operator=(const driver&) = delete;
    driver& operator=(driver&&) = delete;

    inline clock::time_point now();
    inline void step_time();

    inline void asap(event);
    inline event asap();
    inline void at(clock::time_point t, event);
    inline event at(clock::time_point t);
    inline void after(clock::duration d, event);
    inline event after(clock::duration d);

    void loop();
    void clear();

    // introspection
    inline size_t timer_size() const;

    static std::unique_ptr<driver> main;
    static bool clearing;

private:
    friend struct detail::event_body;
    template <typename T> friend struct detail::task_event_awaiter;

    std::deque<std::coroutine_handle<>> ready_;
    std::deque<event> asap_;
    timer_heap<detail::event_handle> timed_;
    clock::time_point now_;
};

}

#include "detail/cotamer_impl.hh"
