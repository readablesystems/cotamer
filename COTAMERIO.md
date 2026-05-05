# Cotamer I/O Programming Manual

Cotamer provides asynchronous I/O through reference-counted file descriptors,
readiness events, and a coroutine-aware mutex. These components let you write
non-blocking network code in a sequential style.


## File descriptors

A `cotamer::fd` is a reference-counted wrapper around a Unix file descriptor.
Constructing a `cotamer::fd` transfers ownership over the corresponding file:
when the last copy of the `cotamer::fd` is destroyed, the underlying file
descriptor is automatically closed.

```cpp
cot::fd rfd(fildes);       // takes ownership of raw fd
cot::fd rfd_copy = rfd;    // shares the same underlying fd
assert(rfd.fileno() == rfd_copy.fileno());
```

| Member                     | Description                                      |
|:---------------------------|:-------------------------------------------------|
| `fd()`                     | construct an invalid fd                          |
| `fd(int fileno)`           | take ownership of a file descriptor              |
| `fileno()`                 | return the file descriptor number                |
| `valid()`, `operator bool` | true if the fd is open                           |
| `close()`                  | close fd                                         |

Closing an `fd`, whether by calling `close()`, by destroying the last copy, or
by cancelling the owning task, triggers all outstanding readiness events on
that fd. This guarantees that coroutines waiting on a closed fd will always
wake up.


## Readiness events

Three free functions create events tied to file descriptor readiness:

| Function                   | Triggers when...                                |
|:---------------------------|:------------------------------------------------|
| `cotamer::readable(f)`     | `::read(f.fileno())` wouldn’t block             |
| `cotamer::writable(f)`     | `::write(f.fileno())` wouldn’t block            |
| `cotamer::closed(f)`       | the fd encounters an error or closes            |

Like all Cotamer events, these are one-shot: once triggered, they stay
triggered. To wait for readability again, call `readable(f)` again.

```cpp
cot::fd rfd(fildes);
cot::set_nonblocking(rfd);

co_await cot::readable(rfd);              // suspend until data available or EOF
char buf[256];
ssize_t n = ::read(rfd.fileno(), buf, sizeof(buf));
```

File descriptors must be in non-blocking mode for readiness events to work
correctly. I/O helpers that return a `cotamer::fd` (`accept`, `tcp_listen`,
`tcp_connect`, `tcp_accept`) automatically put that fd in non-blocking mode;
for file descriptors obtained in other ways, call `cotamer::set_nonblocking`,
which is overloaded for both a raw `int` fd and a `cotamer::fd`.

When a file descriptor closes or experiences a serious error, Cotamer triggers
all three readiness events for that file descriptor. This means that most code
doesn’t need `closed()`. It can be useful nevertheless for coroutines that
passively track files without reading or writing them.


## Byte transfer

The `cotamer::read_once`, `cotamer::write_once`, `cotamer::read`, and
`cotamer::write` coroutines are suspendable wrappers around `::read` and
`::write`.

Transient errors (`EINTR`, `EAGAIN`, and `EWOULDBLOCK`) cause these functions to
retry. On serious errors (anything else), the functions exit early. If any data
had been transferred, they return a short count; but if the error occurred
before data transfer, they return an error indication. The `*once` versions
retry while blocked, while the others retry until all requested bytes are
transferred (or end-of-file or error).

These coroutines return a
[`std::expected`](https://en.cppreference.com/cpp/utility/expected) object,
which encapsulates either a real value or an error code. Specifically, they
return `cotamer::ioresult`, a synonym for `std::expected<size_t,
std::error_code>`. On success, this value encapsulates the number of transferred
bytes; you can access that count with `*result`. On failure, it encapsulates the
`errno` number (in a
[`std::error_code`](https://en.cppreference.com/cpp/error/error_code) object).
Some example uses:

```c++
cot::ioresult result = cot::read(fd, buf, n);

if (result) {
    // transfer succeeded (or end-of-file)
    size_t nbytes = *result;   // fetch number of bytes
} else {
    // serious error
    int errc = result.error().value();
}

if (!result || *result == 0) {
    // no bytes transferred
}

size_t nb = *result;    // will throw exception if `result` is an error
```

Signatures:

| Function | Description |
|:---------|:------------|
| `task<ioresult> read_once(fd, void* buf, size_t count)` | Read until first success |
| `task<ioresult> read(fd, void* buf, size_t count)` | Read until completion |
| `task<ioresult> write_once(fd, const void* buf, size_t count)` | Write until first success |
| `task<ioresult> write(fd, const void* buf, size_t count)` | Write until completion |
| `task<ioresult> writev(fd, const iovec* iov, size_t iovcnt)` | Scatter-gather write until completion |


Here is a complete pipe example:

```cpp
cot::task<> pipe_echo() {
    int piperaw[2];
    pipe(piperaw);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd);
    cot::set_nonblocking(wfd);

    // writer
    auto writer = [&]() -> cot::task<> {
        co_await cot::write(wfd, "hello", 5);
    };
    writer().detach();

    // reader
    char buf[64] = {};
    if (auto r = co_await cot::read_once(rfd, buf, sizeof(buf))) {
        std::print("read {} bytes: {}\n", *r, std::string_view(buf, *r));
    }
}
```


## Sockets

`cotamer::connect` and `cotamer::accept` perform asynchronous versions of the
corresponding socket system calls. `connect` connects a client socket to a
remote address; the coroutine suspends until the connection attempt succeeds.
`accept` suspends until a new connection request arrives on `lfd`; when one
does, it is accepted, put into non-blocking mode, and returned as an `fd`
object. These functions throw `std::system_error` on serious error, rather
than returning an `ioresult`.

To read and write the resulting sockets, prefer `cotamer::send` and
`cotamer::recv`. Although `write` and `read` also work, the `send` and `recv`
versions supply arguments useful for typical socket use; for instance, consider
a socket whose remote peer has shut down; `write` to such a socket may kill the
process via the `SIGPIPE` signal, while `send` just returns an error.

Signatures:

| Function | Description |
|:---------|:------------|
| `task<ioresult> send_once(fd, const void* buf, size_t count)` | Send to first success |
| `task<ioresult> send(fd, const void* buf, size_t count)` | Send to completion |
| `task<ioresult> sendv(fd, const iovec* iov, size_t iovcnt)` | Scatter-gather send to completion |
| `task<ioresult> recv_once(fd, void* buf, size_t count)` | Receive to first success |
| `task<ioresult> recv(fd, void* buf, size_t count)` | Receive to completion |
| `task<> connect(fd, const sockaddr*, socklen_t)` | Connect client socket to address |
| `task<fd> accept(fd)` | Accept new nonblocking socket from listening fd |


## TCP

Higher-level coroutines handle TCP connection setup:

| Function                         | Description                                              |
|:---------------------------------|:---------------------------------------------------------|
| `task<fd> tcp_listen(std::string address [, int backlog])`   | bind and listen; returns a listening `fd`                  |
| `task<fd> tcp_connect(std::string address)`           | connect to a remote address using TCP; returns a connected `fd`    |
| `task<fd> tcp_accept(fd listen_fd)`          | accept a connection on a TCP listening socket            |

Addresses are strings like `"127.0.0.1:8080"`, `"www.google.com:80"`, or
`":8080"`. DNS resolution uses the standard `getaddrinfo(3)` function, and
happens on a background thread to avoid blocking the event loop. `tcp_connect`
and `tcp_accept` both set the `TCP_NODELAY` socket option to disable [Nagle’s
algorithm](https://en.wikipedia.org/wiki/Nagle%27s_algorithm)
([why?](https://brooker.co.za/blog/2024/05/09/nagle.html)). If DNS resolution
fails, the functions throw `std::runtime_error` with an error description; on
other failures, they throw `std::system_error` with `errno` as error code.

```cpp
cot::task<> echo_server() {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:9000");
    while (true) {
        auto cfd = co_await cot::tcp_accept(lfd);
        handle_connection(std::move(cfd)).detach();
    }
}

cot::task<> handle_connection(cot::fd f) {
    char buf[4096];
    while (true) {
        auto n = co_await cot::recv_once(f, buf, sizeof(buf));
        if (!n || *n == 0) {
            break;
        }
        co_await cot::send(f, buf, *n);
    }
}
```


## Mutual exclusion

When multiple coroutines share a file descriptor, a logical operation like
“write this entire message” might require several system calls, with `co_await
writable(f)` suspensions in between. Without protection, another coroutine
could interleave its own writes during those suspensions, corrupting both
messages. A `cotamer::mutex` solves this: a coroutine can hold a mutex for the
duration of its logical operation, ensuring that each multi-call sequence
completes atomically with respect to other coroutines.

Unlike `std::mutex`, which blocks the calling thread, `cotamer::mutex`
*suspends* the calling *coroutine*. Other coroutines on the same thread
continue to run.

Mutex functions are:

| Function            | Description                                            |
|:--------------------|:-------------------------------------------------------|
| `cotamer::mutex m;` | construct mutex                                        |
| `co_await m.lock()` | acquire exclusive access                               |
| `co_await m`        | equivalent to `co_await m.lock()`                      |
| `m.try_lock()`      | try to acquire exclusive access without suspending     |
| `m.unlock()`        | release exclusive access                               |

Cotamer mutexes are thread-safe, so multiple threads can call `m.lock()`
concurrently (only one thread will win). Lock attempts are serviced in FIFO
order: if coroutine A calls `m.lock()` before coroutine B does, then A will
always acquire access before B (unless A is destroyed before acquiring
access).

Since coroutines can be destroyed at any suspension point, explicitly calling
`m.lock()` and `m.unlock()` risks abandoning a mutex in the locked state.
Avoid this with `cotamer::unique_lock`, a guarded lock ownership class
mirroring `std::unique_lock`. Pass the `unique_lock` constructor the ownership
token returned by `co_await m` or `co_await m.lock()`. For instance:

```cpp
struct lockable_fd {
    cot::fd f;
    cot::mutex mutex;
};

cot::task<> write_message(lockable_fd& lfd, std::string message) {
    cot::unique_lock guard(co_await lfd.mutex);
    // critical section — automatically unlocked when guard goes out of scope
    // or coroutine is destroyed. `cot::send` may suspend internally on
    // `writable(lfd.f)`; the guard keeps interleaved writes from other
    // coroutines from corrupting our message across those suspensions.
    co_await cot::send(lfd.f, message.data(), message.size());
}
```

`cotamer::unique_lock` can also be explicitly locked or unlocked (with
`co_await guard.lock()` and `guard.unlock()`), and supports the standard
construction tags:

```cpp
cot::unique_lock guard(m, std::defer_lock);    // don’t lock yet
co_await guard.lock();                         // lock explicitly later

cot::unique_lock guard(m, std::try_to_lock);   // non-blocking attempt
if (guard) {
    // acquired
}

co_await m.lock();
cot::unique_lock guard(m, std::adopt_lock);    // take ownership of existing lock
```

Active attempts to lock a mutex may be cancelled by destroying the
corresponding task. For instance, here, if the timeout fires, the `m.lock()`
attempt is safely removed from the mutex’s queue:

```cpp
auto result = co_await cot::attempt(m.lock(), cot::after(1s));
if (result) {
    cot::unique_lock guard(*result);
    // acquired within 1 second
} else {
    // timed out — lock was not acquired; this coroutine loses its place in line
}
```

### Shared access

The exclusive lock mode is most useful in typical coroutine code, but
`cotamer::mutex` also supports a shared lock mode modelling readers–writer
locking. The shared lock mode uses `*_shared` mutex functions and the
`cotamer::shared_lock` guard.

| Function                   | Description                                     |
|:---------------------------|:------------------------------------------------|
| `co_await m.lock_shared()` | acquire shared access                           |
| `m.try_lock_shared()`      | try to acquire shared access without suspending |
| `m.unlock_shared()`        | release shared access                           |

The FIFO rule also applies to mixtures of shared and exclusive access. If
`mutex m` is acquired in shared mode, but an exclusive lock attempt has
already been enqueued, then another coroutine that calls `m.lock_shared()`
will suspend until the exclusive lock attempt has been serviced. On the other
hand, if no exclusive attempts are waiting, then `m.lock_shared()` will
proceed right away.


## HTTP

Cotamer integrates [llhttp](https://github.com/nodejs/llhttp) to parse and
build HTTP/1.x messages on top of a connected `cotamer::fd`.
`cotamer::http_message` represents a request or response in memory;
`cotamer::http_parser` wraps an `fd` and turns bytes on the wire into
messages and back.

The central class is `cotamer::http_parser`, which can read and write HTTP
messages. This example uses `cot::http_parser` to return the contents of
`/robots.txt` on a given HTTP host:

```c++
#include "cotamer/http.hh"

cot::task<std::string> fetch_robots_txt(std::string host) {
    auto fd = co_await cot::tcp_connect(std::format("{}:80", host));
    cot::http_parser hp(std::move(fd), cot::http_parser::client, host);

    cot::http_message req(HTTP_GET, "/robots.txt");
    auto ticket = co_await hp.send_request(std::move(req));
    co_return (co_await hp.receive(std::move(ticket))).body();
}
```

(The third constructor argument tells `http_parser` to add a `Host:` header to
each outgoing request automatically.)

This uses `cot::http_parser` in server mode to serve HTTP on a given connection:

```cpp
cot::task<> http_connection(cot::fd cfd) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    do {
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            break;                                 // peer closed or parse error
        }
        cot::http_message res;
        res.status_code(200)
            .header("Content-Type", "text/plain")
            .body(std::format("you asked for {}\n", req.url()));
        co_await hp.send(std::move(res));
    } while (hp.should_keep_alive());
}
```


### http_message

A default-constructed `http_message` is a `GET /` request with HTTP/1.1 and
status 200; pass `(method, url)` to construct otherwise. `http_message` uses
[“fluent” accessors](https://en.wikipedia.org/wiki/Fluent_interface) to set
parameters:

```cpp
cot::http_message res;
res.status_code(200)
    .header("Content-Type", "text/plain")
    .body("hello\n");
```

| Member                              | Returns                                         |
|:------------------------------------|:------------------------------------------------|
| `method()`, `method_name()`         | request method                                  |
| `status_code()`                     | response status                                 |
| `url()`                             | full request URL                                |
| `path()`                            | URL path part (no `?...` or `#...`)             |
| `body()`                            | message body                                    |
| `header_begin/end()`                | iterate over headers; each iterator has `.name()` and `.value()` |
| `search_param_begin/end()`          | iterate over `?key=value` parameters            |

If `nlohmann::json` is enabled at build time, `http_message::body(const
nlohmann::json&)` is also available; it serializes the JSON, sets the body,
and adds a `Content-Type: application/json` header if none is already
present.


### Pipelining

HTTP/1.1 supports request pipelining: a client can pass multiple requests over
the connection without waiting for any responses; the server then returns their
responses in order. Cotamer `http_parser` also supports request pipelining, but
some care is required to ensure requests and responses match. The
`http_parser::send_request` coroutine therefore returns a *ticket*, the caller’s
reserved position in line. If you aren’t using pipelining, you can ignore the
ticket. If you are using pipelining, pass the ticket to `receive()` via
`hp.receive(std::move(ticket))` to claim the matching response in line.


For fuller examples covering routing, JSON request and response bodies, error
responses, and a pipelined client, see `examples/jsond.cc` and
`examples/jsond-client.cc`.
