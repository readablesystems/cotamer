#pragma once
#include <cstddef>

// event_handle.hh
//    Declares the cotamer::detail::event_handle class, a smart pointer
//    specialized for Cotamer events. A full definition of this class is
//    required in order to declare the public cotamer::event class.

namespace cotamer {
namespace detail {
struct event_body;
struct quorum_event_body;
template <typename T> struct task_promise;
template <typename T> struct task_awaiter;
template <typename T> struct task_event_awaiter;
template <typename T> struct task_final_awaiter;
struct interest_event_awaiter;

class event_handle {
public:
    event_handle() = default;
    explicit inline event_handle(event_body*) noexcept;
    inline event_handle(const event_handle&) noexcept;
    inline event_handle(event_handle&&) noexcept;
    inline event_handle& operator=(const event_handle&);
    inline event_handle& operator=(event_handle&&) noexcept;
    inline event_handle& operator=(std::nullptr_t);
    inline void swap(event_handle&) noexcept;
    inline ~event_handle();

    explicit operator bool() const { return eb_ != nullptr; }
    inline bool empty() const noexcept;
    inline bool idle() const noexcept;
    event_body* get() const { return eb_; }
    event_body& operator*() const { return *eb_; }
    event_body* operator->() const { return eb_; }

private:
    event_body* eb_ = nullptr;
};

inline void swap(event_handle& a, event_handle& b) noexcept { a.swap(b); }
}
}
