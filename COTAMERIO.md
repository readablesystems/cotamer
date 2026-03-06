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
for file descriptors obtained in other ways, call
`cotamer::set_nonblocking(fileno)` or `cotamer::set_nonblocking(fd)`.

When a file descriptor closes or experiences a serious error, Cotamer triggers
all three readiness events for that file descriptor. This means that most code
doesn’t need `closed()`. It can be useful nevertheless for coroutines that
passively track files without reading or writing them.


## Wrapper functions

The `cotamer::read_once`, `cotamer::write_once`, `cotamer::read`, and
`cotamer::write` coroutines are suspendable wrappers around `::read` and
`::write`. The `*once` versions retry while blocked; the other versions retry
until all requested bytes are transferred (or end-of-file or error). These
functions return the number of bytes transferred (which might be zero at
end-of-file).

Transient errors (`EINTR`, `EAGAIN`, and `EWOULDBLOCK`) cause these functions
to retry. On serious errors (anything else), the functions exit early. If any
data had been transferred, they return a short count; but if the error
occurred before data transfer, they throw a `std::system_error` with the
`errno` number as its `code()`. Recover the errno number with `catch (const
std::system_error& ex) { ... ex.code() ... }`.

Since the functions throw exceptions instead of returning -1 on error, their
return type is `task<size_t>`, not `task<ssize_t>`.

`cotamer::connect` and `cotamer::accept` perform asynchronous versions of the
corresponding socket system calls. `connect` connects a client socket to a
remote address; the coroutine suspends until the connection attempt succeeds.
`accept` suspends until a new connection request arrives on `lfd`; when one
does, it is accepted, put into non-blocking mode, and returned as an `fd`
object. Again, these functions throw `std::system_error` on serious error.

Signatures:

| Function | Description |
|:---------|:------------|
| `task<size_t> read_once(fd, void* buf, size_t count)` | Read until first success |
| `task<size_t> read(fd, void* buf, size_t count)` | Read until completion |
| `task<size_t> write_once(fd, const void* buf, size_t count)` | Write until first success |
| `task<size_t> write(fd, const void* buf, size_t count)` | Write until completion |
| `task<> connect(fd, const sockaddr*, socklen_t)` | Connect client socket to address |
| `task<fd> accept(fd)` | Accept new nonblocking socket from listening fd |

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
    auto r = co_await cot::read_once(rfd, buf, sizeof(buf));
    std::print("read {} bytes: {}\n", r, std::string_view(buf, r));
}
```


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
        auto n = co_await cot::read_once(f, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        co_await cot::write(f, buf, n);
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
    // or coroutine is destroyed
    size_t pos = 0;
    while (pos != message.size()) {
        auto rv = ::write(lfd.f.fileno(), message.data() + pos, message.size() - pos);
        if (rv > 0) {
            pos += rv;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // suspension point: coroutine might be destroyed!
            co_await cot::writable(lfd.f);
        } else {
            throw std::system_error(errno, std::generic_category());
        }
    }
}
```

`cotamer::unique_lock` can also be explicitly locked or unlocked (with
`co_await guard.lock()` and `guard.unlock()`), and supports the standard
construction tags:

```cpp
cot::unique_lock guard(m, std::defer_lock);    // don't lock yet
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
