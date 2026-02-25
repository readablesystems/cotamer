#pragma once
#include "cotamer.hh"
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

// cotamer_io.hh
//    Async I/O primitives built on cotamer::readable() and cotamer::writable().
//    All fds must be non-blocking.

namespace cotamer {

// Reads up to n bytes. Suspends on EAGAIN. Returns bytes read (0 = EOF, -1 = error).
inline task<ssize_t> async_read(const fd& f, void* buf, size_t n) {
    while (true) {
        ssize_t r = ::read(f.fileno(), buf, n);
        if (r >= 0) {
            co_return r;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return -1;
        }
        co_await readable(f);
    }
}

// Writes all n bytes, suspending as needed. Returns total written or -1 on error.
inline task<ssize_t> async_write(const fd& f, const void* buf, size_t n) {
    size_t written = 0;
    const char* p = static_cast<const char*>(buf);
    while (written < n) {
        ssize_t r = ::write(f.fileno(), p + written, n - written);
        if (r > 0) {
            written += r;
            continue;
        }
        if (r == 0) {
            co_return static_cast<ssize_t>(written);
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return -1;
        }
        co_await writable(f);
    }
    co_return static_cast<ssize_t>(written);
}

// Accepts a connection. Returns new fd (with ownership) or invalid fd on error.
inline task<fd> async_accept(const fd& listen_fd) {
    while (true) {
        int raw = ::accept(listen_fd.fileno(), nullptr, nullptr);
        if (raw >= 0) {
            co_return fd(raw);
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return fd();
        }
        co_await readable(listen_fd);
    }
}

// Connects to an address. Suspends until connected.
inline task<int> async_connect(const fd& f, const struct sockaddr* addr, socklen_t len) {
    int r = ::connect(f.fileno(), addr, len);
    if (r == 0) {
        co_return 0;
    }
    if (errno != EINPROGRESS) {
        co_return -1;
    }
    co_await writable(f);
    // Check for connection error
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(f.fileno(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        co_return -1;
    }
    co_return err == 0 ? 0 : -1;
}

} // namespace cotamer
