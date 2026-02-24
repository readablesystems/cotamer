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
template <typename T, bool shared> struct task_mutex_event_awaiter;
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


struct fd_update {
    int fd;
    int mask;
    unsigned epoch;
};

class fd_event_set {
public:
    fd_event_set() = default;
    ~fd_event_set();
    fd_event_set(const fd_event_set&) = delete;
    fd_event_set(fd_event_set&&) = delete;
    fd_event_set& operator=(const fd_event_set&) = delete;
    fd_event_set& operator=(fd_event_set&&) = delete;

    static constexpr unsigned empty_epoch = 0;
    static constexpr unsigned internal_epoch = 1;
    static constexpr unsigned user_epoch = 2;

    inline event_handle watch(int fd, int type);
    inline event_handle take(int fd, int type, unsigned epoch);
    inline std::optional<fd_update> forget(int fd) noexcept;

    inline bool has_update() const noexcept;
    inline std::optional<fd_update> pop_update() noexcept;
    inline std::optional<fd_update> next_nonempty(int fd) const noexcept;

    void clear();

private:
    static constexpr unsigned first_capacity = 128;
    static constexpr unsigned block_capacity = 1024;
    struct fdrec {
        event_handle ev[3];         // 0: readable, 1: writable, 2: closed
        unsigned update_link = 0;   // 0: not updated, -1: end of update list,
                                    // > 0: next updated fd + 1
        unsigned epoch = 0;         // used to avoid epoll API difficulties

        inline int mask() const noexcept {
            return (ev[0] ? 1 : 0) | (ev[1] ? 2 : 0) | (ev[2] ? 4 : 0);
        }
    };

    fdrec* fdrs_ = nullptr;
    unsigned capacity_ = 0;
    unsigned update_link_ = -1;   // -1: no updates, > 0: first updated fd + 1

    void hard_ensure(unsigned fd);
};

struct fd_batch;

}
}
