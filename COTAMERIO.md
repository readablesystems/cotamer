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
by cancelling the owning task, triggers all outstanding readiness events on that
fd. Furthermore, readiness events created on an already-closed (or never-opened)
`fd` trigger immediately. This guarantees that coroutines waiting on a closed fd
will always wake up.


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
`tcp_connect`, `tcp_accept`, `udp_listen`, `udp_connect`, and the `_all`
variants) automatically put that fd in non-blocking mode; for file descriptors obtained in other ways, call
`cotamer::set_nonblocking`, which is overloaded for both a raw `int` fd and a
`cotamer::fd`.

Sockets created by these helpers are also close-on-exec, so child processes
(started with `fork`/`exec`, `posix_spawn`, `system`, …) don’t inherit them.
To hand a socket to a child deliberately, clear the flag with
`cotamer::set_cloexec(f, false)`.

When a file descriptor closes or experiences a serious error, Cotamer triggers
all three readiness events for that file descriptor. This means that most code
doesn’t need `closed()`. It can be useful nevertheless for coroutines that
passively track files without reading or writing them.


## Byte transfer

The `cotamer::read_once`, `cotamer::write_once`, `cotamer::read`, and
`cotamer::write` coroutines are suspendable wrappers around `::read` and
`::write`.

Transient errors (`EINTR`, `EAGAIN`, and `EWOULDBLOCK`) cause these functions to
retry. On serious errors (anything else), the functions exit early, returning
either a short count (if any data has been transferred) or an error indication
(if the error occurred before transfer). The `once` versions retry only while
blocked; the others loop until all bytes are transferred (or end-of-file or
error).

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
cot::ioresult result = co_await cot::read(fd, buf, n);

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
than returning an `ioresult`. `accept` does not throw for errors that affect
only one pending connection, such as `ECONNABORTED`; it moves on to the next
connection. If the process runs out of file descriptors or memory (`EMFILE`,
`ENFILE`, `ENOBUFS`, `ENOMEM`), `accept` waits and retries, backing off from
5ms to 1s, so an accept loop survives temporary resource exhaustion.

To read and write the resulting sockets, prefer `cotamer::send` and
`cotamer::recv`. Although `write` and `read` also work, the `send` and `recv`
versions supply arguments useful for typical socket use. For instance, if a
socket’s remote peer has shut down, `write` might kill the process via the
`SIGPIPE` signal, whereas `send` just returns an error.

`cotamer::send` and `cotamer::recv` behave like the `send` and `recv` system
calls: they retry only while blocked, rather than looping until all data is
transferred. Use `send_all` or set `MSG_WAITALL` in `flags` to loop.

Signatures:

| Function | Description |
|:---------|:------------|
| `task<ioresult> send(fd, const void* buf, size_t count[, int flags])` | Send to first success |
| `task<ioresult> sendv(fd, const iovec* iov, size_t iovcnt[, int flags])` | Scatter-gather send to first success |
| `task<ioresult> send_all(fd, const void* buf, size_t count[, int flags])` | Send to completion |
| `task<ioresult> sendv_all(fd, const iovec* iov, size_t iovcnt[, int flags])` | Scatter-gather send to completion |
| `task<ioresult> sendto(fd, const void* buf, size_t count, const sockaddr* addr, socklen_t[, int flags])` | Send datagram to explicit destination |
| `task<ioresult> sendmsg(fd, const msghdr* msg, int flags)` | Send message |
| `task<ioresult> recv(fd, void* buf, size_t count[, int flags])` | Receive to first success |
| `task<ioresult> recvfrom(fd, void* buf, size_t count, sockaddr* addr, socklen_t*[, int flags])` | Receive datagram and capture sender address |
| `task<ioresult> recvmsg(fd, msghdr* msg, int flags)` | Receive message |
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
fails, the functions throw `std::system_error` whose code has category
`cot::getaddrinfo_error_category()` and an `EAI_*` value (such as
`EAI_NONAME`); on other failures, they throw `std::system_error` with `errno`
as error code.

```cpp
cot::task<> echo_server() {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:9000");
    while (true) {
        auto cfd = co_await cot::tcp_accept(lfd);
        handle_connection(std::move(cfd)).detach();
    }
}

cot::task<> handle_connection(cot::fd f) {
    while (true) {
        char buf[4096];
        auto n = co_await cot::recv(f, buf, sizeof(buf));
        if (!n || *n == 0) {
            break;
        }
        co_await cot::send_all(f, buf, *n);
    }
}

cot::task<> echo_client(std::string address, std::string message) {
    auto f = co_await cot::tcp_connect(address);
    co_await cot::send_all(f, message.data(), message.size());
    char buf[4096];
    auto n = co_await cot::recv(f, buf, sizeof(buf));
    if (n) {
        std::print("echo: {}\n", std::string_view(buf, *n));
    }
}
```

The `tcp_listen_all(address)` function can return multiple `fd`s, supporting
`address`es that correspond to multiple address families. For instance,
`localhost` has an IPv4 and an IPv6 version. Use it like this:

```c++
cot::task<> one_echo_server(cot::fd lfd) {
    while (true) {
        auto cfd = co_await cot::tcp_accept(lfd);
        handle_connection(std::move(cfd)).detach();
    }
}

cot::task<> all_echo_servers() {
    auto lfds = co_await cot::tcp_listen_all("localhost:9000");
    for (auto& lfd : lfds) {
        one_echo_server(std::move(lfd)).detach();
    }
}
```


## UDP

UDP is a datagram protocol: a socket returned by `cot::udp_listen` exchanges
datagrams with any peer. Each `recvfrom` returns exactly one whole datagram
and reports the sender’s address; `sendto` directs a reply back.

| Function                                    | Description                               |
|:--------------------------------------------|:------------------------------------------|
| `task<fd> udp_listen(std::string address)`  | bind a UDP socket to `address`            |
| `task<fd> udp_connect(std::string address)` | create a UDP socket connected to `address`; plain `send`/`recv` go to that peer |

```cpp
cot::task<> udp_echo_server() {
    auto sock = co_await cot::udp_listen("127.0.0.1:9000");
    while (true) {
        sockaddr_storage src{};
        socklen_t srclen = sizeof(src);
        char buf[65536];
        auto n = co_await cot::recvfrom(sock, buf, sizeof(buf),
                                        reinterpret_cast<sockaddr*>(&src), &srclen);
        if (!n) {
            break;
        }
        co_await cot::sendto(sock, buf, *n,
                             reinterpret_cast<sockaddr*>(&src), srclen);
    }
}

cot::task<> udp_echo_client(std::string address, std::string message) {
    auto sock = co_await cot::udp_connect(address);
    co_await cot::send(sock, message.data(), message.size());
    char buf[65536];
    auto n = co_await cot::recv(sock, buf, sizeof(buf));
    if (n) {
        std::print("echo: {}\n", std::string_view(buf, *n));
    }
}
```

As with TCP, a `udp_listen_all(address)` function can return multiple `fd`s,
and errors are reported the same way.


## Signals

`cot::at_signal(signo)` returns an event that triggers when the process next
receives signal `signo`. Invalid signal numbers, `SIGKILL`, and `SIGSTOP` throw
`std::invalid_argument`.

```cpp
cot::task<> reload_on_sighup() {
    while (true) {
        co_await cot::at_signal(SIGHUP);
        reload_configuration();
    }
}
```

Cotamer installs a do-nothing signal handler for `signo` while some
`at_signal(signo)` event is pending. If all such events trigger and their
handlers don’t recreate them, the signal’s previous handler is reinstalled.

Cotamer doesn’t remember how many times a signal is delivered, so multiple
signals sent in close succession may be combined into a single event
notification.

A pending `at_signal` event keeps `loop()` running. If a signal handler should
not keep the driver running, use `cot::at_signal_weak(signo)` instead. `loop()`
will return if its driver has no outstanding events or tasks except weak
signals.


## Mutual exclusion

A logical operation like “write a message to a file descriptor” might require
several system calls, with `co_await writable(f)` suspensions in between. If
several coroutines share that file descriptor, another coroutine’s writes can
interleave during those suspensions, corrupting both messages. This is a
mutual exclusion problem.

The `cotamer::mutex` synchronization object can be locked by one coroutine to
obtain exclusive access. Any other coroutine that attempts to lock the mutex
will suspend until the mutex is unlocked. Unlike `std::mutex`, which blocks the
calling thread, `cotamer::mutex` *suspends* the calling *coroutine*; other
coroutines on the same thread continue to run.

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
    // Critical section. `cot::send_all` may suspend internally on
    // `writable(lfd.f)`; the guard keeps interleaved writes from other
    // coroutines from corrupting our message across those suspensions.
    // The guard automatically unlocks the mutex when the coroutine exits.
    co_await cot::send_all(lfd.f, message.data(), message.size());
}
```

`cotamer::unique_lock` can also be explicitly locked or unlocked (with
`co_await guard.lock()` and `guard.unlock()`), and supports the standard
construction tags:

```cpp
cot::unique_lock guard(m, std::defer_lock);    // don’t lock yet
co_await guard.lock();                         // lock explicitly later

cot::unique_lock guard(m, std::try_to_lock);   // non-blocking attempt
if (guard.owns_lock()) {
    // acquired; `if (guard)` would also work
}

co_await m.lock();
cot::unique_lock guard(m, std::adopt_lock);    // take ownership of existing lock
```

`unique_lock::lock()` attempts are safe to cancel, so `cot::first` or
`cot::race` can add timeouts to them. For instance, here, if the timeout fires
first, the lock attempt is safely removed from the mutex’s queue. (Using
`m.lock()` in a race is inherently unsafe.)

```cpp
cot::unique_lock guard(m, std::defer_lock);
co_await cot::first(guard.lock(), cot::after(1s));
if (guard.owns_lock()) {
    // acquired within 1 second
} else {
    // timed out — lock was not acquired
}
```

### Shared access

Most coroutine code only needs exclusive access, but `cotamer::mutex` also
supports a shared lock mode modelling readers–writer locking. The shared lock
mode uses `*_shared` mutex functions and the `cotamer::shared_lock` guard.

| Function                   | Description                                     |
|:---------------------------|:------------------------------------------------|
| `co_await m.lock_shared()` | acquire shared access                           |
| `m.try_lock_shared()`      | try to acquire shared access without suspending |
| `m.unlock_shared()`        | release shared access                           |

Mixtures of shared and exclusive lock requests are serviced in FIFO order. If
`mutex m` is acquired in shared mode, but an exclusive lock attempt has already
been enqueued, then another coroutine that calls `m.lock_shared()` will suspend
until the exclusive lock attempt has been serviced. On the other hand, if no
exclusive attempts are waiting, then `m.lock_shared()` will proceed right away.


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

HTTP requests and responses are represented by the `http_message` type.
`http_message` uses [“fluent” accessors](https://en.wikipedia.org/wiki/Fluent_interface) to set parameters:

```cpp
cot::http_message res;
res.status_code(200)
    .header("Content-Type", "text/plain")
    .body("hello\n");
```

| Member                        | Accesses                              | Message type | Mode       |
|:------------------------------|:--------------------------------------|:-------------|:-----------|
| `method()`, `method_name()`   | request method                        | Request      | Read/write |
| `status_code()`               | response status code (e.g. 200, 404)  | Response     | Read/write |
| `url()`                       | request URL                           | Request      | Read/write |
| `body()`                      | message body                          | Any          | Read/write |
| `header(name, value)`         | header                                | Any          | Write      |
| `header(name)`                | header by name                        | Any          | Read       |
| `remove_header(name)`         | remove all headers named `name`       | Any          | Write      |
| `remove_header_if(pred)`      | remove headers where `pred(name, value)` is true | Any | Write    |
| `path()`                      | URL path part (no `?...` or `#...`)   | Request      | Read       |
| `decoded_path()`              | decoded, normalized path (see below)  | Request      | Read       |
| `upgrade()`                   | true for CONNECT or protocol upgrade  | Any          | Read       |
| `header_begin/end()`          | iterate over headers; each iterator has `.name()` and `.value()` | Any | Read |
| `search_param_begin/end()`    | iterate over `?name=value` parameters | Request      | Read       |
| `search_param(name)`          | search parameter by name              | Request      | Read       |

If `nlohmann::json` is enabled at build time, `http_message::body(const
nlohmann::json&)` is also available; it serializes the JSON, sets the body,
and adds a `Content-Type: application/json` header if none is already
present.

Header names must be HTTP tokens, and header values, status messages, and
URLs may not contain CR, LF, or NUL (URLs also may not contain spaces or
tabs). Adding an invalid header throws `std::invalid_argument`, as does
sending a message with an invalid status message, URL, or status code (which
must be between 100 and 999).

`path()` returns the raw path, exactly as received. `decoded_path()`
percent-decodes the path and resolves `.` and `..` segments, which is useful
when mapping a URL to a file. It returns `std::nullopt` if the URL is
invalid, the path contains `%00` or `%2F`, or a `..` segment would rise above
the root.


### Structured header values

`cotamer/http_fields.hh` helps parse header values that follow the
grammar of RFC 9110 §5.6. Both helpers are views; the string they parse must
outlive them.

- `cot::strings::http_value_list(value)` iterates over a comma-separated list
  (as in `Connection`, `Upgrade`, or `Accept-Encoding`). Entries are trimmed
  of whitespace, empty entries are skipped, and commas inside quoted strings
  don’t split. `icontains(token)` tests whether a list contains `token`,
  ignoring case.
- `cot::strings::http_parameter_list(value)` iterates over a
  semicolon-separated parameter list (as in `Content-Type` or
  `Sec-WebSocket-Extensions`). Its iterator has `name()`, `has_value()`,
  `raw_value()` (quoted values keep their quotes), and `value()` (quotes and
  escapes removed by `cot::strings::http_unquote`).

```cpp
std::string conn = req.header("connection");   // the view needs a real string
if (cot::strings::http_value_list(conn).icontains("upgrade")) { ... }
```


### Framing

`http_parser` frames the bodies it sends. A message with a body (set with
`body()` or `append_body()`, or received with one) is sent with a
`Content-Length` that matches the body; any `Content-Length` or
`Transfer-Encoding` headers on the message are replaced. A response without
a body gets `Content-Length: 0`, unless it has its own `Content-Length` or
`Transfer-Encoding` header. `send_response` never sends a body with a 1xx,
204, or 304 response. When responding to a `HEAD` request, it sends headers
but no body; if the response has a body, its `Content-Length` reports the
body’s length. To stream a response body, set `Transfer-Encoding: chunked`
without setting a body, then use `send_response_chunk` and
`send_response_end_chunk`. (Empty chunks are ignored.)

A received message keeps its headers as they arrived, including
`Transfer-Encoding: chunked`, but its body is decoded. Thanks to the framing
rules, it can be forwarded as is. A proxy should also remove hop-by-hop
headers: `Connection`, any headers named in `Connection`, `Keep-Alive`,
`Proxy-Connection`, `TE`, `Trailer`, and `Upgrade`. For example:

```cpp
#include "cotamer/http_fields.hh"
using cot::strings::http_value_list;

auto req = co_await downstream.receive();
std::string conn = req.header("connection");
http_value_list hop("connection, keep-alive, proxy-connection, te, trailer, upgrade");
req.remove_header_if([&](std::string_view name, std::string_view) {
    return hop.icontains(name) || http_value_list(conn).icontains(name);
});
auto ticket = co_await upstream.send_request(std::move(req));
```

(This checks every header against every name in `Connection`. A hostile peer
can list many names there; a production proxy should limit their number or
use a hash set.)

A chunked message’s trailers are received as ordinary headers.

On the client side, `http_parser` remembers the method of each request sent
with `send_request`. It knows that a response to `HEAD` has no body, even if
it has a nonzero `Content-Length`, and that a 2xx response to `CONNECT`
starts a tunnel: the response has `upgrade()` true, and tunneled bytes that
arrived with the response remain in `receive_buffer()`. A response with
neither `Content-Length` nor `Transfer-Encoding` ends when the server closes
the connection.


### Limits and timeouts

`http_parser` limits the messages it receives:

| Setting                                  | Default                        |
|:-----------------------------------------|:-------------------------------|
| `set_max_header_size(n)`: request or status line plus headers | 64 KiB |
| `set_max_body_size(n)`                   | 16 MiB                         |
| `set_receive_timeout(d)`: time for one `receive()` to complete | 60s in server mode; none in client mode |

Pass `cotamer::duration::max()` to `set_receive_timeout` to disable the
timeout. In server mode, the timeout also bounds how long an idle keep-alive
connection waits for its next request.

When a limit is exceeded, `receive()` returns an error, `hp.ok()` is false,
`hp.error()` is `HPE_USER`, and `hp.failure()` says which limit failed:
`header_too_large` (suggested response 431), `body_too_large` (413), or
`timeout` (408). A failed parser cannot receive more messages, and
`should_keep_alive()` returns false. For example:

```cpp
auto req = co_await hp.receive();
if (hp.failure() == cot::http_parser::failure_type::body_too_large) {
    co_await hp.send(cot::http_message(413).header("Connection", "close"));
}
if (!hp.ok()) {
    co_return;
}
```

`receive()` may be cancelled, for instance with `cot::attempt`. If it is
cancelled between messages, the parser remains usable. If it is cancelled
partway through a message, the parser cannot resume; it fails with
`failure_type::cancelled`.


### Upgrades

A CONNECT request, or a request with `Connection: upgrade` and an `Upgrade`
header, ends the HTTP portion of a connection. `receive()` returns that
request with `upgrade()` true. Afterward, `should_keep_alive()` returns false,
later `receive()` calls fail with `HPE_PAUSED_UPGRADE`, and any bytes that
followed the request remain in `receive_buffer()`. A server that supports the
upgrade can respond with `101 Switching Protocols`, then use `take_file()`
and `receive_buffer()` to continue with the new protocol. Other servers
should respond normally; the connection then closes.


### Pipelining

HTTP/1.1 supports request pipelining: a client can pass multiple requests over
the connection without waiting for any responses; the server then returns their
responses in order. Cotamer `http_parser` also supports request pipelining, but
some care is required to ensure requests and responses match. The
`http_parser::send_request` coroutine therefore returns a *ticket*, the caller’s
reserved position in line. If you aren’t using pipelining, you can ignore the
ticket: dropping a ticket gives up its place in line, and `receive()` without a
ticket takes the next place. If you are using pipelining, pass the ticket to
`receive()` via `hp.receive(std::move(ticket))` to claim the matching response
in line.


For fuller examples covering routing, JSON request and response bodies, error
responses, and a pipelined client, see `examples/jsond.cc` and
`examples/jsond-client.cc`.
