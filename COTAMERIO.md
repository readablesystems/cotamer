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
int piperaw[2];
pipe(piperaw);
cot::fd rfd(piperaw[0]);       // takes ownership of raw fd
cot::fd rfd_copy = rfd;        // shares the same underlying fd
assert(rfd.fileno() == rfd_copy.fileno());
```

| Member                     | Description                                      |
|:---------------------------|:-------------------------------------------------|
| `fd()`                     | construct an invalid fd                          |
| `fd(int rawfd)`            | take ownership of a raw file descriptor          |
| `fileno()`                 | return the raw file descriptor number            |
| `valid()`, `operator bool` | true if the fd is open                           |
| `close()`                  | close fd                                         |

Closing an `fd`—whether by calling `close()`, by destroying the last copy, or
by cancelling the owning task—triggers all outstanding readiness events on that
fd. This guarantees that coroutines waiting on a closed fd will always wake up.


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
cot::fd rfd(piperaw[0]);
cot::set_nonblocking(rfd.fileno());

co_await cot::readable(rfd);              // suspend until data available
char buf[256];
ssize_t n = ::read(rfd.fileno(), buf, sizeof(buf));
```

File descriptors must be in non-blocking mode for readiness events to work
correctly. The I/O helpers automatically put their fds in non-blocking mode; for
file descriptors you construct in other ways, call
`cotamer::set_nonblocking(rawfd)`.


## I/O helpers

Cotamer provides convenience coroutines that combine readiness events with
system calls, so you don't have to write the retry loop yourself:

| Function                                    | Description                                        |
|:--------------------------------------------|:---------------------------------------------------|
| `read_once(f, buf, n)` | read up to `n` bytes; suspends on `EAGAIN`; returns bytes read, 0 for EOF, or negative errno |
| `write_once(f, buf, n)` | write up to `n` bytes; suspends on `EAGAIN`; returns bytes written or negative errno |
| `write(f, buf, n)` | write all `n` bytes, suspending as needed; returns total written or negative errno |

These are defined in `cotamer/io.hh` (included by `cotamer.hh`).

Here is a complete pipe example:

```cpp
cot::task<> pipe_echo() {
    int piperaw[2];
    pipe(piperaw);
    cot::fd rfd(piperaw[0]), wfd(piperaw[1]);
    cot::set_nonblocking(rfd.fileno());
    cot::set_nonblocking(wfd.fileno());

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
| `tcp_listen(address, backlog)`   | bind and listen; returns a listening `fd`                |
| `tcp_connect(address)`           | connect to a remote address; returns a connected `fd`    |
| `tcp_accept(listen_fd)`          | accept a connection; returns a new `fd` with `TCP_NODELAY` |
| `accept(listen_fd)`              | accept a connection without setting socket options       |
| `connect(f, addr, len)`          | low-level async connect on an existing `fd`              |

Addresses are strings like `"127.0.0.1:8080"` or `":8080"`. DNS resolution
happens on a background thread to avoid blocking the event loop.

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
        if (n <= 0) break;
        co_await cot::write(f, buf, n);
    }
}
```


## Mutual exclusion

When multiple coroutines share a file descriptor, a logical operation like
“write this entire message” might require several system calls, with `co_await
writable(f)` suspensions in between. Without protection, another coroutine could
interleave its own writes during those suspensions, corrupting both messages. A
`cotamer::mutex` solves this: a coroutine can hold a mutex for the duration of
its logical operation, ensuring that each multi-call sequence completes
atomically with respect to other coroutines.

Unlike `std::mutex`, which blocks the calling thread, `cotamer::mutex`
*suspends* the calling *coroutine*. Other coroutines on the same thread continue
to run.

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
always acquire access before B (unless A is destroyed before acquiring access).

Since coroutines can be destroyed at any suspension point, explicitly calling
`m.lock()` and `m.unlock()` risks abandoning a mutex in the locked state. Avoid
this with `cotamer::unique_lock`, a guarded lock ownership class mirroring
`std::unique_lock`. Pass the `unique_lock` constructor the ownership token
returned by `co_await m` or `co_await m.lock()`. For instance:

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
            co_await cot::writable(lfd.f);   // suspension point: coroutine might be destroyed!
        } else {
            /* ... handle error ... */
        }
    }
}
```

`cotamer::unique_lock` can also be explicitly locked or unlocked (with `co_await
guard.lock()` and `guard.unlock()`), and supports the standard construction
tags:

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

Active attempts to lock a mutex may be cancelled by destroying the corresponding
task. For instance, here, if the timeout fires, the `m.lock()` attempt is safely
removed from the mutex’s queue:

```cpp
auto result = co_await cot::attempt(m.lock(), cot::after(1s));
if (result) {
    cot::unique_lock guard(*result);
    // acquired within 1 second
} else {
    // timed out — lock was not acquired; this coroutine’s place in line
    // is skipped
}
```

### Shared access

The exclusive lock mode is most useful in typical coroutine code, but
`cotamer::mutex` also supports a shared lock mode modelling readers–writer
locking. The shared lock mode uses `*_shared` mutex functions and the
`cotamer::shared_lock` guard.

| Function            | Description                                            |
|:--------------------|:-------------------------------------------------------|
| `co_await m.lock_shared()` | acquire shared access                           |
| `m.try_lock_shared()`      | try to acquire shared access without suspending |
| `m.unlock_shared()`        | release shared access                           |

The FIFO rule also applies to mixtures of shared and exclusive access. If `mutex
m` is acquired in shared mode, but an exclusive lock attempt has already been
enqueued, then another coroutine that calls `m.lock_shared()` will suspend until
the exclusive lock attempt has been serviced. On the other hand, if no exclusive
attempts are waiting, then `m.lock_shared()` will proceed right away.
