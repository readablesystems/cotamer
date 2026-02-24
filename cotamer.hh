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

    inline bool triggered() const noexcept;
    inline bool empty() const noexcept;
    inline bool idle() const noexcept;

    inline bool trigger();

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
//    A single global driver is stored in `driver::main`. The free functions
//    `now()`, `after()`, `loop()`, etc. delegate to it.

using system_time_point = std::chrono::system_clock::time_point;
using steady_time_point = std::chrono::steady_clock::time_point;
using duration = std::chrono::steady_clock::duration;

enum class fdi { read = 0, write = 1, close = 2 };

class driver {
public:
    driver();
    ~driver();
    driver(const driver&) = delete;
    driver(driver&&) = delete;
    driver& operator=(const driver&) = delete;
    driver& operator=(driver&&) = delete;

    inline void set_real_time(bool real_time);

    inline system_time_point now() noexcept;        // current system time (might go backwards)
    inline steady_time_point steady_now() noexcept; // time since boot (monotonic)
    inline void step_time() noexcept;

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
    inline event fd(int fd, fdi type);

    int close_fd(int fd);
    void forget_fd(int fd);

    void loop();
    void clear();
    bool clearing() const { return clearing_; }

    // introspection
    inline size_t timer_size() const;

    static thread_local std::unique_ptr<driver> main;

private:
    friend struct detail::event_body;
    template <typename T> friend struct detail::task_final_awaiter;
    friend void set_real_time(bool);

    system_time_point virtual_epoch_;
    steady_time_point snow_;
    bool clearing_ = false;
    bool real_time_ = false;
    std::deque<detail::event_handle> asap_;
    timer_heap<detail::event_handle> timed_;

    static constexpr uint32_t df_lock = 1;
    static constexpr uint32_t df_nonempty = 2;
    std::atomic<uint32_t> lock_ = 0;
    std::atomic<int> wakefd_ = -1;
    std::deque<detail::event_handle> migrate_;

    int pollfd_ = -1;
    int epoll_wakefd_ = -1;
    unsigned nfdctl_ = 0;
    std::vector<uint64_t> fdctl_;
    detail::fd_event_set fds_;

    static std::atomic<bool> real_time;

    inline uint32_t lock();
    inline void unlock(uint32_t flags);
    void migrate_asap(detail::event_handle eh);

    inline int pollfd();
    void apply_fd_update(detail::fd_batch&, const detail::fd_update&);
    bool watch_fds(detail::fd_batch&, duration timeout);
};


// Time and scheduling functions (operate on driver::main).

inline void set_real_time(bool real_time);

inline system_time_point now() noexcept;
inline steady_time_point steady_now() noexcept;
inline void step_time() noexcept;

inline event asap();                   // triggers before next time step
inline event after(duration);          // triggers after a delay
template <typename Rep, typename Period>
inline event after(const std::chrono::duration<Rep, Period>&);
inline event at(steady_time_point);    // triggers at an absolute time
inline event at(system_time_point);    // triggers at an absolute system time

inline event readable(int fd);         // triggers when `read(fd)` won't block
inline event writable(int fd);         // triggers when `write(fd)` won't block
inline event closed(int fd);           // triggers when `fd` errors or closes

inline void loop();                    // run event loop until quiescent
inline int close_fd(int fd);           // close `fd` and trigger all events
inline void forget_fd(int fd);         // drop any events on `fd`
inline void clear();                   // cancel all pending events
void reset();                          // destroy and recreate driver


// mutex, mutex_event, unique_lock, shared_lock
//    Event-driven mutual exclusion for coroutines. `mutex` provides exclusive
//    or shared access to a resource controlled by task suspension. A task can
//    `co_await mutex.lock()` to obtain the lock. The `unique_lock` and
//    `shared_lock` classes are RAII wrappers resembling their standard
//    counterparts. For instance:
//        cot::unique_lock guard(co_await mutex.lock());
//    When that guard goes out of scope the mutex will automatically unlock.

class mutex;

template <bool shared>
struct locked_mutex_t {
    mutex* m;
};

template <bool shared>
class mutex_event {
public:
    using mutex_type = cotamer::mutex;

    ~mutex_event() = default;
    mutex_event(const mutex_event&) = default;
    mutex_event(mutex_event&&) = default;
    mutex_event& operator=(const mutex_event&) = default;
    mutex_event& operator=(mutex_event&&) = default;

    inline bool triggered() const noexcept;

    inline mutex_type* mutex() const noexcept;
    inline const detail::event_handle& handle() const& noexcept;
    inline detail::event_handle&& handle() && noexcept;

private:
    friend class mutex;

    mutex_type* m_;
    detail::event_handle ep_;

    inline mutex_event(mutex_type*);
    inline bool trigger();
};

class mutex {
public:
    inline mutex() = default;
    mutex(const mutex&) = delete;
    mutex(mutex&&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex& operator=(mutex&&) = delete;
    inline ~mutex() = default;

    [[nodiscard]] inline mutex_event<false> lock();
    [[nodiscard]] inline bool try_lock();
    inline void unlock();

    [[nodiscard]] inline mutex_event<true> lock_shared();
    [[nodiscard]] inline bool try_lock_shared();
    inline void unlock_shared();

private:
    using latch_type = unsigned;
    static constexpr latch_type mf_latch = 1;         // latch bit for multithreading
    static constexpr latch_type mf_next_excl = 2;     // next in line wants exclusive
    static constexpr latch_type mf_next_shared = 4;   // next in line wants shared
    static constexpr latch_type mfm_next = 6;         // either mf_next_excl or mf_next_shared
    static constexpr latch_type mf_lock_excl = 8;     // exclusive lock held
    static constexpr latch_type mf_lock_shared = 16;  // added once per shared lock held

    // protects waiters_, tracks information about lock
    std::atomic<latch_type> latch_ = 0;          // see mf_ constants
    // queue of events waiting for mutex; see `lock_impl`
    std::deque<detail::event_handle> waiters_;

    inline latch_type latch();
    inline void unlatch(latch_type);
    inline bool allow(bool shared, latch_type) const noexcept;
    inline bool waiter_shared(const detail::event_handle&) const noexcept;
    [[nodiscard]] inline latch_type notify_locked(latch_type);
    void lock_impl(bool shared, detail::event_handle& ep);
    void unlock_impl(bool shared);
};


// unique_lock, shared_lock
//    RAII lock guards for cotamer::mutex.

class unique_lock {
public:
    using mutex_type = cotamer::mutex;

    unique_lock() noexcept = default;
    inline unique_lock(mutex_type&, std::defer_lock_t) noexcept;
    inline unique_lock(mutex_type&, std::try_to_lock_t) noexcept;
    inline unique_lock(mutex_type&, std::adopt_lock_t) noexcept;
    inline unique_lock(locked_mutex_t<false> token) noexcept;
    inline unique_lock(unique_lock&&) noexcept;
    inline unique_lock& operator=(unique_lock&&) noexcept;
    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;
    inline ~unique_lock();

    [[nodiscard]] inline task<> lock();
    [[nodiscard]] inline bool try_lock();
    inline void unlock();

    inline void swap(unique_lock&) noexcept;
    inline mutex_type* release() noexcept;

    mutex_type* mutex() const noexcept { return m_; }
    bool owns_lock() const noexcept { return owned_; }
    explicit operator bool() const noexcept { return owned_; }

private:
    mutex_type* m_ = nullptr;
    bool owned_ = false;
};

class shared_lock {
public:
    using mutex_type = cotamer::mutex;

    shared_lock() noexcept = default;
    inline shared_lock(mutex_type&, std::defer_lock_t) noexcept;
    inline shared_lock(mutex_type&, std::try_to_lock_t) noexcept;
    inline shared_lock(mutex_type&, std::adopt_lock_t) noexcept;
    inline shared_lock(locked_mutex_t<true> token) noexcept;
    inline shared_lock(shared_lock&& x) noexcept;
    inline shared_lock& operator=(shared_lock&& x) noexcept;
    shared_lock(const shared_lock&) = delete;
    shared_lock& operator=(const shared_lock&) = delete;
    inline ~shared_lock();

    [[nodiscard]] inline task<> lock();
    [[nodiscard]] inline bool try_lock();
    inline void unlock();

    inline void swap(shared_lock&) noexcept;
    inline mutex_type* release() noexcept;

    mutex_type* mutex() const noexcept { return m_; }
    bool owns_lock() const noexcept { return owned_; }
    explicit operator bool() const noexcept { return owned_; }

private:
    mutex_type* m_ = nullptr;
    bool owned_ = false;
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

}

#include "cotamer/cotamer_impl.hh"
