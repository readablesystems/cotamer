#pragma once
#include "cotamer.hh"
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#if !COTAMER_USE_KQUEUE && !COTAMER_USE_EPOLL && !COTAMER_USE_POLL
# if defined(__APPLE__) || defined(__FreeBSD__)
#  define COTAMER_USE_KQUEUE 1
# elif defined(__linux__)
#  define COTAMER_USE_EPOLL 1
# else
#  define COTAMER_USE_POLL 1
# endif
#endif
#if COTAMER_USE_KQUEUE
# include <sys/event.h>
#elif COTAMER_USE_EPOLL
# include <sys/epoll.h>
# include <sys/eventfd.h>
#else
# include <poll.h>
#endif

// cotamer/io.hh
//    Async I/O primitives built on cotamer::readable() and cotamer::writable().

namespace cotamer {

inline void throw_errno() {
    throw std::system_error(errno, std::generic_category());
}

// Set a raw file descriptor to non-blocking mode.
inline void set_nonblocking(int rawfd) {
    int flags = fcntl(rawfd, F_GETFL, 0);
    if (flags < 0 || fcntl(rawfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw_errno();
    }
}

// Reads up to count bytes. Suspends on EAGAIN. Returns bytes read.
inline task<size_t> read_once(const fd& f, void* buf, size_t count) {
    while (true) {
        ssize_t r = count ? ::read(f.fileno(), buf, count) : 0;
        if (r >= 0) {
            co_return r;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            throw_errno();
        }
        co_await readable(f);
    }
}

// Writes up to count bytes. Suspends on EAGAIN. Returns bytes written.
inline task<size_t> write_once(const fd& f, const void* buf, size_t count) {
    while (true) {
        ssize_t r = count ? ::write(f.fileno(), buf, count) : 0;
        if (r >= 0) {
            co_return r;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            throw_errno();
        }
        co_await writable(f);
    }
}

// Reads all n bytes, suspending as needed. Returns bytes read.
inline task<size_t> read(const fd& f, void* buf, size_t count) {
    char* p = static_cast<char*>(buf);
    size_t nr = 0;
    while (true) {
        ssize_t r = count ? ::read(f.fileno(), p + nr, count - nr) : 0;
        if (r > 0) {
            nr += r;
            if (nr == count) {
                break;
            }
        } else if (r == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            if (nr > 0) {
                break;
            }
            throw_errno();
        }
        co_await readable(f);
    }
    co_return nr;
}

// Writes all n bytes, suspending as needed. Returns bytes written.
inline task<size_t> write(const fd& f, const void* buf, size_t count) {
    const char* p = static_cast<const char*>(buf);
    size_t nw = 0;
    while (true) {
        ssize_t r = count ? ::write(f.fileno(), p + nw, count - nw) : 0;
        if (r > 0) {
            nw += r;
            if (nw == count) {
                break;
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            if (nw > 0) {
                break;
            }
            throw_errno();
        }
        co_await writable(f);
    }
    co_return nw;
}

// Connects to an address. Suspends until connected. Throws on error.
inline task<> connect(const fd& f, const struct sockaddr* addr, socklen_t len) {
    int r = ::connect(f.fileno(), addr, len), err = 0;
    if (r == 0) {
        co_return;
    } else if (errno != EINPROGRESS) {
        err = errno;
    } else {
        co_await writable(f);
        // Check for connection error
        socklen_t errlen = sizeof(err);
        if (getsockopt(f.fileno(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            err = errno;
        }
    }
    if (err != 0) {
        throw std::system_error(err, std::generic_category());
    }
}

// Accepts a connection. Returns new fd (with ownership). Throws on error.
inline task<fd> accept(const fd& listen_fd) {
    while (true) {
        int rawfd = ::accept(listen_fd.fileno(), nullptr, nullptr);
        if (rawfd >= 0) {
            set_nonblocking(rawfd);
            co_return fd(rawfd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            throw_errno();
        }
        co_await readable(listen_fd);
    }
}

inline task<fd> tcp_accept(const fd& listen_fd) {
    auto f = co_await accept(listen_fd);
    // disable Nagle’s algorithm
    int flag = 1;
    setsockopt(f.fileno(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    co_return std::move(f);
}


namespace detail {

struct fd_batch {
#if COTAMER_USE_KQUEUE
    using system_event_type = struct kevent;
    static constexpr size_t capacity = 256;
    struct kevent ev[capacity];
    int changes = 0;
#elif COTAMER_USE_EPOLL
    using system_event_type = struct epoll_event;
    static constexpr size_t capacity = 256;
    struct epoll_event ev[capacity];
#else
    using system_event_type = struct pollfd;
    small_vector<struct pollfd, 16> ev;
#endif
    int size = 0;
    int index = 0;

    static inline int mask_out(int mask) noexcept;
    static inline int mask_in(const system_event_type&) noexcept;

    inline void add(int pollfd, const fd_update& fdu, int old_mask);
    inline void clear(int pollfd);
    inline std::optional<fd_update> pop() noexcept;
    void print_changes();
};

inline void fd_batch::clear(int pollfd) {
#if COTAMER_USE_KQUEUE
    if (changes) {
        kevent(pollfd, ev, changes, nullptr, 0, nullptr);
        changes = 0;
    }
#else
    (void) pollfd;
#endif
}

inline fd_event_set::~fd_event_set() {
    if (capacity_ != 0) {
        std::destroy(fdrs_, fdrs_ + capacity_);
        std::allocator<fdrec>().deallocate(fdrs_, capacity_);
    }
}

} // namespace detail


inline int driver::pollfd() {
    if (pollfd_ < 0) {
        hard_pollfd();
    }
    return pollfd_;
}

inline void driver::notify_close(int base_fd) {
    if (auto pair = fds_.check_fd_close(base_fd)) {
        detail::fd_batch batch;
        apply_fd_update(batch, {base_fd, 0, pair->second});
        batch.clear(pollfd());
        pair->first->remove_listener(this);
    }
}

} // namespace cotamer
