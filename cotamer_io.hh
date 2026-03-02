#pragma once
#include "cotamer.hh"
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

// cotamer_io.hh
//    Async I/O primitives built on cotamer::readable() and cotamer::writable().
//    All fds must be non-blocking.

namespace cotamer {

// Reads up to n bytes. Suspends on EAGAIN. Returns bytes read
// (0 = EOF; negative = error code).
inline task<ssize_t> read_once(const fd& f, void* buf, size_t n) {
    while (true) {
        ssize_t r = n ? ::read(f.fileno(), buf, n) : 0;
        if (r >= 0) {
            co_return r;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return -errno;
        }
        co_await readable(f);
    }
}

// Writes up to n bytes. Suspends on EGAIN. Returns bytes written or
// negative error code.
inline task<ssize_t> write_once(const fd& f, const void* buf, size_t n) {
    while (true) {
        ssize_t r = n ? ::write(f.fileno(), buf, n) : 0;
        if (r >= 0) {
            co_return r;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return -errno;
        }
        co_await writable(f);
    }
}

// Writes all n bytes, suspending as needed. Returns total written or
// negative error code.
inline task<ssize_t> write(const fd& f, const void* buf, size_t n) {
    size_t nw = 0;
    const char* p = static_cast<const char*>(buf);
    while (true) {
        ssize_t r = n > nw ? ::write(f.fileno(), p + nw, n - nw) : 0;
        if (r > 0) {
            nw += r;
        }
        if (nw == n) {
            co_return nw;
        } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return nw ? nw : -errno;
        }
        co_await writable(f);
    }
}

// Accepts a connection. Returns new fd (with ownership) or invalid fd on error.
inline task<fd> accept(const fd& listen_fd) {
    while (true) {
        int raw = ::accept(listen_fd.fileno(), nullptr, nullptr);
        if (raw >= 0) {
            co_return fd(raw);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return fd(-errno);
        }
        co_await readable(listen_fd);
    }
}

// Connects to an address. Suspends until connected.
inline task<int> connect(const fd& f, const struct sockaddr* addr, socklen_t len) {
    int r = ::connect(f.fileno(), addr, len);
    if (r == 0) {
        co_return 0;
    } else if (errno != EINPROGRESS) {
        co_return -errno;
    }
    co_await writable(f);
    // Check for connection error
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(f.fileno(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        co_return -errno;
    }
    co_return err == 0 ? 0 : -err;
}

} // namespace cotamer
