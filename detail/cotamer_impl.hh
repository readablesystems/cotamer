#pragma once
#include "detail/small_vector.hh"

// cotamer_impl.hh
//    This file defines the gory details of the Cotamer implementation,
//    including C++ coroutine machinery.

namespace cotamer {
namespace detail {

// mark a listener as a quorum
constexpr uintptr_t lf_quorum = uintptr_t(1);

// event_body::flags_
constexpr uint32_t f_quorum = 1;        // this is a quorum_event_body
constexpr uint32_t f_want_interest = 2; // a transitive quorum member needs interest{}
constexpr uint32_t f_interest = 16;     // this quorum has 1 interest{}
                                        // (added once per interest{})


// task_promise<T>
//    This structure is part of the C++ coroutine machinery. Coroutines don’t
//    actually use the stack for local variables; they can’t, because they can
//    suspend themselves and resume later. When a coroutine is first called, the
//    C++ runtime allocates a heap structure for the function’s locals. That
//    heap structure also contains this *promise*. The promise interface is
//    defined by the C++ language standard; the runtime calls its methods in
//    specific situations, such as when a `co_await` expression is evaluated.

template <typename T>
struct task_promise {
    // Functions required by the C++ runtime
    // - Initialize the task<T> return value that manages the coroutine:
    inline task<T> get_return_object() noexcept;
    // - Behavior when coroutine starts (here, run eagerly):
    std::suspend_never initial_suspend() noexcept { return {}; }
    // - Handle `co_await E` for different `E` types:
    task_event_awaiter<T> await_transform(event ev);
    inline task_event_awaiter<T> await_transform(interest);
    inline interest_event_awaiter await_transform(interest_event);
    template <typename Aw>
    Aw&& await_transform(Aw&& aw) noexcept { return std::forward<Aw>(aw); }
    // - Handle `co_return V` or throwing an exception in the coroutine:
    void return_value(T value) { result_.template emplace<1>(std::move(value)); }
    void unhandled_exception() noexcept { result_.template emplace<2>(std::current_exception()); }
    // - Behavior after coroutine exits:
    task_final_awaiter<T> final_suspend() noexcept;
    // - Export coroutine return value to `co_await`er:
    inline T result();

    // Our own additions
    inline event_handle& make_interest();
    bool detached_ = false;
    bool has_interest_ = false;
    event_handle completion_;
    event_handle interest_;
    std::variant<std::monostate, T, std::exception_ptr> result_;
    std::coroutine_handle<> continuation_;
};

template <typename T>
inline task<T> task_promise<T>::get_return_object() noexcept {
    return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
}

template <typename T>
T task_promise<T>::result() {
    if (result_.index() == 2) {
        std::rethrow_exception(std::move(std::get<2>(result_)));
    }
    return std::move(std::get<1>(result_));
}


// task_promise<void>
//    Similar, but no value is returned.

template <>
struct task_promise<void> {
    inline task<void> get_return_object() noexcept;
    std::suspend_never initial_suspend() noexcept { return {}; }
    task_event_awaiter<void> await_transform(event ev);
    inline task_event_awaiter<void> await_transform(interest);
    inline interest_event_awaiter await_transform(interest_event);
    template <typename Aw>
    Aw&& await_transform(Aw&& aw) noexcept { return std::forward<Aw>(aw); }
    void return_void() noexcept { }
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
    void result() {
        if (exception_) {
            std::rethrow_exception(std::move(exception_));
        }
    }
    inline task_final_awaiter<void> final_suspend() noexcept;

    inline event_handle& make_interest();
    bool detached_ = false;
    bool has_interest_ = false;
    std::coroutine_handle<> continuation_;
    event_handle completion_;
    event_handle interest_;
    std::exception_ptr exception_;
};

inline task<void> task_promise<void>::get_return_object() noexcept {
    return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}


// task_awaiter<T>
//    This structure is also part of the C++ coroutine machinery. “Awaiter”
//    objects are created as part of `co_await` expression evaluation; the
//    C++ runtime calls their methods to determine whether to suspend, how
//    to handle a suspension (including what to run next), and how to resume.
//
//    task_awaiter<T> awaits a task. We also define task_final_awaiter<T>,
//    which handles the implicit final suspension when a coroutine completes;
//    task_event_awaiter<T>, which awaits an event; and a few others.

template <typename T>
struct task_awaiter {
    // - Return true if `co_await` should not suspend
    bool await_ready() noexcept {
        return self_.done();
    }
    // - Suspend this coroutine and return the next coroutine to execute
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        self_.promise().continuation_ = awaiting;
        if (self_.promise().interest_) {
            self_.promise().interest_->trigger();
        }
        return std::noop_coroutine();
    }
    // - Resume this coroutine, returning the `co_await` expression’s result
    T await_resume() {
        return self_.promise().result();
    }

    std::coroutine_handle<task_promise<T>> self_;
};


// Awaiter for the implicit final suspension when a coroutine completes.
template <typename T>
struct task_final_awaiter {
    bool await_ready() noexcept {
        return false;
    }
    inline std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise<T>> self) noexcept {
        auto& promise = self.promise();
        // trigger completion event, since the task is done
        if (promise.completion_) {
            promise.completion_->trigger();
        }
        // if another coroutine wants this task’s result, run them immediately
        if (promise.continuation_) {
            return std::exchange(promise.continuation_, nullptr);
        }
        // destroy if detached and then return to event loop
        if (promise.detached_) {
            self.destroy();
        }
        return std::noop_coroutine();
    }
    void await_resume() noexcept {
    }
};

template <typename T>
inline task_final_awaiter<T> task_promise<T>::final_suspend() noexcept {
    return {};
}

inline task_final_awaiter<void> task_promise<void>::final_suspend() noexcept {
    return {};
}


// make_event: converts various types into events.

template <typename T>
inline event make_event(T resumable) {
    co_await resumable;
}

inline event& make_event(event& ev) {
    return ev;
}

inline event&& make_event(event&& ev) {
    return std::move(ev);
}

inline event make_event(interest);


// event_body
//    The heap-allocated state behind an event. Managed by reference-counted
//    event_handle smart pointers. Each event_body has a list of *listeners*
//    (coroutines or quorum bodies) that are notified when the event triggers.

struct event_body {
    event_body() = default;
    event_body(const event_body&) = delete;
    event_body(event_body&&) = delete;
    event_body& operator=(const event_body&) = delete;
    event_body& operator=(event_body&&) = delete;
    ~event_body() {
        if (!idle()) {
            trigger();
        }
    }

    // This event can be garbage collected: it has triggered, or it has no
    // listeners and no other references.
    bool empty() const noexcept {
        return listeners_.empty_capacity()
            || (listeners_.empty() && refcount_.load(std::memory_order_relaxed) == 1);
    }

    // This event has no listeners.
    bool idle() const noexcept {
        return listeners_.empty();
    }

    // The event has triggered.
    bool triggered() const noexcept {
        return listeners_.empty_capacity();
        // In its initial state, listeners_ has nonzero capacity; we set its
        // capacity to 0 only in triggered().
    }

    void add_listener(uintptr_t listener) {
        // A listener is either a `coroutine_handle<T>::address()` or the
        // address of a `quorum_event_body`. Quorum bodies are distinguished by
        // setting the `lf_quorum` bit, bit 1; this is safe because coroutines
        // and quorum bodies are both aligned.
        assert(listener && !triggered());
        listeners_.push_back(listener);
    }

    bool remove_listener(uintptr_t listener) {
        // Remove a listener (which might have been added multiple times).
        bool found = false;
        for (auto it = listeners_.begin(); it != listeners_.end(); ) {
            if (*it == listener) {
                *it = listeners_.back();
                listeners_.pop_back();
                found = true;
            } else {
                ++it;
            }
        }
        return found;
    }

    inline void trigger();


    std::atomic<uint32_t> refcount_ = 1;
    uint32_t flags_ = 0;
    small_vector<uintptr_t, 3> listeners_;
};


// quorum_event_body
//    A subclass of event_body. Implements `any()` and `all()` by tracking
//    member events, counting the number that have triggered, and triggering its
//    own event (the event_body base type) once a quorum is reached.
//
//    The `f_interest` and `f_want_interest` flags implement an optimization
//    that avoids allocating separate memory for `interest{}`.

struct quorum_event_body : event_body {
    template<typename... Es>
    quorum_event_body(size_t quorum, Es&&... es)
        : quorum_(quorum) {
        flags_ |= f_quorum;
        (add_member(std::forward<Es>(es)), ...);
        if (triggered_ >= quorum_) {
            trigger();
        }
    }

    ~quorum_event_body() {
        for (auto& mem : members_) {
            mem->remove_listener(listener_id());
        }
    }

    uintptr_t listener_id() const {
        return reinterpret_cast<uintptr_t>(this) | lf_quorum;
    }

    void add_member(event_handle eh) {
        if (!eh || eh->triggered()) {
            ++triggered_;
            return;
        }
        eh->add_listener(listener_id());
        if (eh->flags_ & f_want_interest) {
            flags_ |= f_want_interest;
        }
        members_.push_back(std::move(eh));
    }

    template <typename E>
    inline void add_member(E&& e) {
        add_member(make_event(std::forward<E>(e)).handle());
    }

    inline void add_member(interest) {
        flags_ = (flags_ + f_interest) | f_want_interest;
    }

    // Called by a member event when it triggers.
    void trigger_member(event_body* e) {
        if (triggered()) {
            return;
        }
        for (auto it = members_.begin(); it != members_.end(); ) {
            if (it->get() == e) {
                ++triggered_;
                *it = std::move(members_.back());
                members_.pop_back();
            } else {
                ++it;
            }
        }
        if (triggered_ >= quorum_) {
            trigger();
        }
    }

    inline void fix_want_interest(event_handle& ievent);


    small_vector<event_handle, 3> members_;
    uint32_t triggered_ = 0;
    uint32_t quorum_;
};


inline void event_body::trigger() {
    // Triggering a quorum erases its member list.
    if (flags_ & f_quorum) {
        auto qbody = static_cast<quorum_event_body*>(this);
        for (auto& mem : qbody->members_) {
            mem->remove_listener(qbody->listener_id());
        }
        qbody->members_.clear();
    }
    // Activate listeners: schedule coroutines and inform quorum events.
    // But we can’t actually inform quorum events directly! It may be that
    // the last reference to this event is held by one of its quorum listeners;
    // when we call `trigger_member` on that listener, its quorum might be
    // satisfied, which would drop that reference and `delete this`.
    // So save a stack copy of the quorum listeners that will survive our
    // own deletion.
    small_vector<quorum_event_body*, 2> qe;
    for (auto listener : listeners_) {
        if (listener & lf_quorum) {
            qe.push_back(reinterpret_cast<quorum_event_body*>(listener & ~lf_quorum));
        } else {
            driver::main->ready_.push_back(std::coroutine_handle<>::from_address(reinterpret_cast<void*>(listener)));
        }
    }
    // Mark this event as triggered (not just empty).
    listeners_.clear_capacity();
    // Finally, inform our quorum listeners. During this loop `this` might
    // be freed.
    for (auto listener : qe) {
        listener->trigger_member(this);
    }
}


// event_handle implementation
//    Reference-counted smart pointer for event_body

inline event_handle::event_handle(event_body* eb) noexcept
    : eb_(eb) {
}

inline event_handle::event_handle(const event_handle& x) noexcept
    : eb_(x.eb_) {
    if (eb_) {
        eb_->refcount_.fetch_add(1, std::memory_order_relaxed);
    }
}

inline event_handle::event_handle(event_handle&& x) noexcept
    : eb_(std::exchange(x.eb_, nullptr)) {
}

inline event_handle& event_handle::operator=(std::nullptr_t) {
    event_handle tmp;
    std::swap(eb_, tmp.eb_);
    return *this;
}

inline event_handle& event_handle::operator=(const event_handle& x) {
    if (this != &x) {
        event_handle tmp(x);
        std::swap(eb_, tmp.eb_);
    }
    return *this;
}

inline event_handle& event_handle::operator=(event_handle&& x) noexcept {
    if (this != &x) {
        event_handle tmp(std::move(x));
        std::swap(eb_, tmp.eb_);
    }
    return *this;
}

inline event_handle::~event_handle() {
    if (eb_ && eb_->refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Check if this event_body is a quorum_event_body
        if (eb_->flags_ & f_quorum) {
            delete static_cast<quorum_event_body*>(eb_);
        } else {
            delete eb_;
        }
    }
}

inline void event_handle::swap(event_handle& x) noexcept {
    auto tmp = eb_;
    eb_ = x.eb_;
    x.eb_ = tmp;
}

inline bool event_handle::empty() const noexcept {
    return !eb_ || eb_->empty();
}



// task_event_awaiter<T>
//    Awaiter for `co_await event` inside a task.

template <typename T>
struct task_event_awaiter {
    event_handle eh_;
    uintptr_t coroutine_;

    ~task_event_awaiter() {
        if (coroutine_ && !eh_->remove_listener(coroutine_)) {
            // Our containing coroutine is being destroyed while suspended, but
            // `eh_` has already triggered and scheduled our coroutine_handle on
            // the driver's ready queue. Avoid use-after-free by removing the
            // coroutine from the driver's queue.
            auto coh = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(coroutine_));
            std::erase(driver::main->ready_, coh);
        }
    }
    bool await_ready() noexcept {
        return !eh_ || eh_->triggered();
    }
    bool await_suspend(std::coroutine_handle<task_promise<T>> awaiting) noexcept {
        event_body* eb = eh_.get();
        // We’re about to suspend on `eb`. Optimization: Apply interest{} here,
        // just before suspending. That application might trigger `eb`.
        if (eb->flags_ & f_want_interest) {
            static_cast<quorum_event_body*>(eb)->fix_want_interest(awaiting.promise().make_interest());
            if (eb->triggered()) {
                return false;
            }
        }
        coroutine_ = reinterpret_cast<uintptr_t>(awaiting.address());
        eb->add_listener(coroutine_);
        return true;
    }
    void await_resume() {
        coroutine_ = 0;
        // This code helps recover memory when clearing a driver (for instance,
        // if a test exits early). The clearing process triggers all
        // outstanding events and unblocks their waiting coroutines, but those
        // might have other coroutines waiting for their results, rather than
        // events. We destroy the whole chain by forcing the event-unblocked
        // coroutines to throw an exception; that exception is propagated
        // through their awaiters.
        if (driver::clearing) {
            throw clearing_error{};
        }
    }
};

template <typename T>
inline task_event_awaiter<T> task_promise<T>::await_transform(event ev) {
    return task_event_awaiter<T>{std::move(ev).handle(), 0};
}

inline task_event_awaiter<void> task_promise<void>::await_transform(event ev) {
    return task_event_awaiter<void>{std::move(ev).handle(), 0};
}


// Support `interest{}` and `interest_event{}`

template <typename T>
inline event_handle& task_promise<T>::make_interest() {
    if (!has_interest_) {
        interest_ = event_handle(new event_body);
        has_interest_ = true;
    }
    return interest_;
}

inline event_handle& task_promise<void>::make_interest() {
    if (!has_interest_) {
        interest_ = event_handle(new event_body);
        has_interest_ = true;
    }
    return interest_;
}

template <typename T>
inline task_event_awaiter<T> task_promise<T>::await_transform(interest) {
    return task_event_awaiter<T>{make_interest(), 0};
}

inline task_event_awaiter<void> task_promise<void>::await_transform(interest) {
    return task_event_awaiter<void>{make_interest(), 0};
}

// make_event(interest)
//    Wrap bare `interest{}` in a single-member quorum. This is used when
//    `interest{}` appears as the sole argument to any()/all().

inline event make_event(interest) {
    auto q = new detail::quorum_event_body(1);
    q->flags_ |= f_interest | f_want_interest;
    return event_handle(q);
}

// interest_event_awaiter: for `co_await interest_event{}`

struct interest_event_awaiter {
    event_handle handle_;
    bool await_ready() noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept { }
    event await_resume() { return event(std::move(handle_)); }
};

template <typename T>
inline interest_event_awaiter task_promise<T>::await_transform(interest_event) {
    return interest_event_awaiter{make_interest()};
}

inline interest_event_awaiter task_promise<void>::await_transform(interest_event) {
    return interest_event_awaiter{make_interest()};
}

// fix_want_interest(ievent)
//    Called when this quorum and/or its transitive members need to apply
//    interest{}. The `ievent` is the lazily-created event_handle representing
//    interest; it may have already triggered.

inline void quorum_event_body::fix_want_interest(event_handle& ievent) {
    assert((flags_ & (f_quorum | f_want_interest)) == (f_quorum | f_want_interest));
    flags_ &= ~f_want_interest;
    if (triggered()) {
        return;
    }
    // Apply local interest (this quorum has one or more `interest{}` members)
    while (flags_ >= f_interest) {
        add_member(ievent);
        flags_ -= f_interest;
    }
    if (triggered_ >= quorum_) {
        trigger();
        return;
    }
    // Apply transitive interest. Just as with `event-body::trigger()`, we
    // cannot call `mem.fix_want_interest` directly -- there is a chance that
    // `mem.fix_want_interest` triggers that member, which in turn triggers us
    // and/or removes the last reference to `this`, deleting `this`. So make a
    // stack copy first.
    small_vector<event_handle, 3> wi_members;
    for (auto& mem : members_) {
        if (mem->flags_ & f_want_interest) {
            wi_members.push_back(mem);
        }
    }
    for (auto& mem : wi_members) {
        static_cast<quorum_event_body*>(mem.get())->fix_want_interest(ievent);
    }
}

}


// event methods

inline event::event()
    : ep_(new detail::event_body) {
}

inline event::event(detail::event_handle ev)
    : ep_(std::move(ev)) {
}

inline event::event(std::nullptr_t) {
}

inline bool event::empty() const noexcept {
    return !ep_ || ep_->empty();
}

inline bool event::idle() const noexcept {
    return !ep_ || ep_->idle();
}

inline bool event::triggered() const noexcept {
    return !ep_ || ep_->triggered();
}

inline void event::trigger() {
    if (ep_) {
        std::exchange(ep_, nullptr)->trigger();
    }
}

inline const detail::event_handle& event::handle() const& {
    return ep_;
}

inline detail::event_handle&& event::handle() && {
    return std::move(ep_);
}


// task methods

template <typename T>
inline task<T>::task(handle_type handle) noexcept
    : handle_(handle) {
}

template <typename T>
inline task<T>::task(task&& x) noexcept
    : handle_(std::exchange(x.handle_, nullptr)) {
}

template <typename T>
inline task<T>& task<T>::operator=(task&& x) noexcept {
    if (this != &x) {
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(x.handle_, nullptr);
    }
    return *this;
}

template <typename T>
inline task<T>::~task() {
    if (handle_) {
        handle_.destroy();
    }
}

template <typename T>
inline bool task<T>::done() {
    return handle_ && handle_.done();
}

template <typename T>
inline event task<T>::completion() {
    if (done()) {
        return event(nullptr);
    }
    auto& p = handle_.promise();
    if (!p.completion_) {
        p.completion_ = detail::event_handle(new detail::event_body);
    }
    return event(p.completion_);
}

template <typename T>
inline void task<T>::start() {
    if (done()) {
        return;
    }
    auto& p = handle_.promise();
    if (p.interest_) {
        p.interest_->trigger();
    } else {
        p.has_interest_ = true;
    }
}

template <typename T>
inline void task<T>::detach() {
    if (!handle_) {
        return;
    }
    handle_.promise().detached_ = true;
    if (handle_.done()) {
        handle_.destroy();
    }
    handle_ = nullptr;
}

template <typename T>
inline detail::task_awaiter<T> task<T>::operator co_await() const noexcept {
    return detail::task_awaiter<T>{handle_};
}


// driver methods

inline clock::time_point driver::now() {
    return now_;
}

inline void driver::step_time() {
    now_ += clock::duration{1};
}

inline void driver::asap(event e) {
    asap_.push_back(std::move(e));
}

inline event driver::asap() {
    event e;
    asap(e);
    return e;
}

inline void driver::at(clock::time_point t, event e) {
    timed_.emplace(t, std::move(e).handle());
}

inline event driver::at(clock::time_point t) {
    if (t <= now_) {
        return event(nullptr);
    }
    event e;
    at(t, e);
    return e;
}

inline void driver::after(clock::duration d, event e) {
    at(now() + d, std::move(e));
}

inline event driver::after(clock::duration d) {
    return at(now() + d);
}


// time functions

inline clock::time_point now() {
    return driver::main->now();
}

inline void step_time() {
    driver::main->step_time();
}

inline event asap() {
    return driver::main->asap();
}

inline event at(clock::time_point t) {
    return driver::main->at(t);
}

inline event after(clock::duration d) {
    return driver::main->after(d);
}


// Event combinators

// any(), all()
//    Multi-argument forms create a quorum_event_body. Single-argument forms
//    pass through to make_event (no quorum needed). Zero-argument forms
//    return an already-triggered event.

template <typename E0, typename... Es>
inline event any(E0 e0, Es&&... es) {
    auto q = new detail::quorum_event_body(1, std::forward<E0>(e0), std::forward<Es>(es)...);
    return detail::event_handle(q);
}

template <typename E>
inline event any(E&& e) {
    return detail::make_event(std::forward<E>(e));
}

inline event any() {
    // any() with no arguments returns an already-triggered event.
    // An alternate design would treat any() as an untriggered event (like
    // how false is the identity for logical or).
    return event(nullptr);
}


template <typename... Es>
inline event all(Es&&... es) {
    auto q = new detail::quorum_event_body(sizeof...(Es), std::forward<Es>(es)...);
    return detail::event_handle(q);
}

template <typename E>
inline event all(E&& e) {
    return detail::make_event(std::forward<E>(e));
}

inline event all() {
    return event(nullptr);
}


// attempt(t, e...)
//    Runs a `task<T>` (the first argument) with cancellation (the other
//    arguments). Returns `task<std::optional<T>>`, which is `nullopt` if the
//    task was cancelled.

template <typename T, typename... Es>
task<std::optional<T>> attempt(task<T> t, Es&&... es) {
    if (!t.done()) {
        t.start();
        co_await any(t.completion(), std::forward<Es>(es)...);
    }
    if (t.done()) {
        co_return co_await t;
    }
    co_return std::nullopt;
}

template <typename T, typename... Es>
task<std::optional<T>> attempt(task<std::optional<T>> t, Es&&... es) {
    if (!t.done()) {
        t.start();
        co_await any(t.completion(), std::forward<Es>(es)...);
    }
    if (t.done()) {
        co_return co_await t;
    }
    co_return std::nullopt;
}

template <typename... Es>
task<std::optional<std::monostate>> attempt(task<void> t, Es&&... es) {
    if (!t.done()) {
        t.start();
        co_await any(t.completion(), std::forward<Es>(es)...);
    }
    if (t.done()) {
        co_await t;
        co_return std::monostate{};
    }
    co_return std::nullopt;
}


// driver functions

inline void loop() {
    driver::main->loop();
}

inline void clear() {
    driver::main->clear();
}

inline size_t driver::timer_size() const {
    return timed_.size();
}

}
