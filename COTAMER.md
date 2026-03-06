# Cotamer Programming Manual

Cotamer is a C++23 coroutine library for cooperative multitasking with
deterministic virtual time. Programs are structured around tasks and events.


## Tasks

A **task** is a function that can suspend processing and resume later—in
technical terms, a [**coroutine**](https://en.wikipedia.org/wiki/Coroutine).
Tasks use two special C++ keywords:

- `co_await E` — pause until `E` is ready, then continue.
- `co_return value` — finish the task and return a value.

A function that is a task must return the type `cotamer::task<T>`, where `T`
is the type of the `co_return` value. (Tasks that produce no value return
`cotamer::task<>`.)

Here is a complete program with two tasks:

```cpp
#include "cotamer.hh"
#include <print>
namespace cot = cotamer;
using namespace std::chrono_literals;

cot::task<int> slow_add(int a, int b) {
    co_await cot::after(1h);          // pause for 1 hour in virtual time
    co_return a + b;
}

cot::task<> main_task() {
    std::print("{}: starting main_task\n", cot::now());
    int v = co_await slow_add(3, 4);  // suspend until slow_add finishes
    std::print("{}: slow_add returns {}\n", cot::now(), v);
}

int main() {
    auto t = main_task();
    cot::loop();                      // run until all tasks finish
}

// Outputs: (note that the initial time is fixed)
// 2021-10-12 20:21:09.000000: starting main_task
// 2021-10-12 21:21:09.000000: slow_add returns 7
```

A task begins running as soon as it is called, and keeps running until it hits
a `co_await`. Then it suspends and other tasks get a chance to run. When the
awaited event or task becomes ready, the task resumes where it left off.

`co_await t` on a `task<T>` produces the task's return value. If the task
threw an exception, `co_await` rethrows it in the awaiting coroutine. At most
one coroutine can `co_await` a given task.

The `main` function starts `main_task`, which starts `slow_add`. Both
`slow_add` and `main_task` then suspend themselves, `slow_add` waiting on a
timer and `main_task` waiting on `slow_add`. `cotamer::loop()` then advances
virtual time and runs unblocked tasks until there’s nothing left to do.


## Events

The key benefit of coroutine programming is that functions can suspend
themselves while waiting for external occurrences. In Cotamer, **events**
represent these occurrences.

Each event starts in the **untriggered** state and then transitions to the
**triggered** state. Once triggered, an event stays triggered forever: events
are one-shot notifications. A task can wait for an event with `co_await e`.

Cotamer functions can create events associated with specific future
occurrences:

| Function                   | Returns an event that triggers...           |
|:---------------------------|:--------------------------------------------|
| `cotamer::after(duration)` | after the given duration elapses            |
| `cotamer::at(time_point)`  | at the given absolute time                  |
| `cotamer::asap()`          | as soon as possible (next loop iteration)   |
| `cotamer::readable(fd)`    | when `::read(fd)` wouldn’t block            |
| `cotamer::writable(fd)`    | when `::write(fd)` wouldn’t block           |
| `cotamer::closed(fd)`      | when `fd` errors or closes                  |

An event can also be created explicitly with `cotamer::event e` and triggered
with `e.trigger()`. The `e.triggered()` function tests whether an event has
triggered yet.

Copies of an event object refer to the same underlying occurrence: triggering
one copy triggers all of them. Events are automatically reference counted, and
most event operations are thread-safe—it works to construct, trigger, and await
the same event on three different threads. (Event assignment with `operator=` is
not thread-safe.)

When multiple events trigger at the same logical time, they run in the order
they were registered:

```cpp
cot::task<> awaiter(int n, cot::event e) {
    co_await e;
    std::print("{} ", n);
}

int main() {
    auto t0 = awaiter(0, cot::asap());
    auto t1 = awaiter(1, cot::asap());
    auto t2 = awaiter(2, cot::after(5ms));
    auto t3 = awaiter(3, cot::after(10ms));
    auto t4 = awaiter(4, cot::after(10ms));
    auto t5 = awaiter(5, cot::after(5ms));     // earlier expiration time
    cot::loop();
    std::print("\n");
}

// always prints `0 1 2 5 3 4`
```


## Combinators: `any`, `all`, and `attempt`

`cotamer::any()` and `cotamer::all()` combine multiple events into one; you
can supply as many events as you like.

```cpp
// Resume after 1 hour (whichever event fires first)
co_await cot::any(cot::after(1h), cot::after(10h));

// Resume after 10 hours (wait for both)
co_await cot::all(cot::after(1h), cot::after(10h));
```

`cotamer::attempt(task, event...)` runs a task with cancellation: if any of
the `event`s trigger before the task completes, the task is destroyed.
`co_await attempt(task<T>, ...)` returns a `std::optional<T>`, which either
contains the return value from the task or `std::nullopt` if the task was
cancelled. `attempt` is a natural fit for timeouts:

```cpp
auto result = co_await cot::attempt(slow_task(), cot::after(1h));
if (result) {
    use(*result);    // task completed within an hour
} else {
    // task did not complete before the timeout
}
```


## Task lifetime

Coroutine data, such as local variables, is normally tied to the lifetime of
the corresponding `task<T>` object. When the `task` goes out of scope (or is
cancelled by `cotamer::attempt`), all data associated with its coroutine is
destroyed too.

```cpp
cot::task<> printer(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::asap();
    std::print("printer({}) completed\n", i);
}

int main() {
    printer(0);             // `task` destroyed → coroutine destroyed
    auto t = printer(1);    // `task` preserved → coroutine lives
    cot::loop();
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(1) completed
```

The `task::detach()` function breaks the link between a running coroutine and
its `task` object, allowing the coroutine to run to completion even after the
`task` is destroyed. The coroutine’s memory is automatically freed when it
returns.

```cpp
int main() {
    printer(0).detach();    // `task` detached → coroutine lives
    auto t = printer(1);    // `task` preserved → coroutine lives
    cot::loop();
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(0) completed
// printer(1) completed
```


## Event loop

Cotamer runs coroutines from its event loop, `cotamer::loop()`. Every
application thread has its own thread-local driver
(`cotamer::driver::current`); free functions like `cotamer::loop()` call into
that driver. The `loop` function repeatedly runs unblocked tasks, triggers
expired timer events, and checks for file descriptor events, blocking as
appropriate. It runs until there’s nothing left for it to do: no unblocked
tasks, no timers, and no file descriptor interest.

A `cotamer::driver_guard` token keeps `cotamer::loop()` alive and processing
events even if the loop has no other work. This is useful when suspending on
an event managed outside of Cotamer. For example, this function creates a
non-blocking wrapper around the blocking `stat()` system call. The
`driver_guard` prevents Cotamer from terminating until the `nonblocking_stat`
call resolves.

```cpp
cot::task<struct stat> nonblocking_stat(std::string path) {
    cot::event notifier;           // communicate from thread to coroutine
    cot::driver_guard guard;       // prevent early exit from event loop
    // receive return value from thread
    // (The shared_ptr avoids undefined behavior if the coroutine is cancelled
    // while the thread is still running.)
    struct result_struct { struct stat st; int status; int err; };
    auto result = std::make_shared<result_struct>();

    std::thread([=] () mutable {
        result->status = stat(path.c_str(), &result->st);
        result->err = errno;
        notifier.trigger();
    }).detach();

    co_await notifier;             // coroutine waits for thread
    if (result->status != 0) {
        throw std::system_error(result->err, std::generic_category());
    }
    co_return result->st;
}
```

Alternatively, call `cotamer::keepalive(e)` to keep the current driver loop
running until `event e` triggers.

The `cotamer::clear()` function causes a driver loop to exit even if it has
outstanding events. `cotamer::clear()` unregisters all file descriptor events
and destroys outstanding coroutines. This can be useful for testing.

The `cotamer::poll()` function runs the driver once, processing the current
batch of ASAP events, file descriptor updates, and timers in non-blocking
fashion. `cotamer::poll()` returns true if the driver still has outstanding
work. `cotamer::loop()` behaves like `while (cotamer::poll()) {}`, but uses
the CPU more efficiently since `cotamer::loop()` can block.


## Clocks

By default, Cotamer uses **deterministic virtual time**. Each driver starts
running at a fixed timestamp, and time appears to jump forward whenever the
system would block. For instance, this program completes instantaneously, even
though it reports that over a year has elapsed:

```cpp
cot::task<> long_delay() {
    std::print("{}: good morning\n", cot::now());
    co_await cot::after(10000h);
    std::print("{}: good evening\n", cot::now());
}

int main() {
    long_delay().detach();
    cot::loop();
}

// Outputs:
// 2021-10-12 20:21:09.000000: good morning
// 2022-12-03 12:21:09.000000: good evening
```

Virtual time lets us run efficient tests on code that involves long timeouts,
since the timeouts happen instantaneously in real time. Furthermore, since
time is virtual and scheduling order is deterministic, no aspects of system
behavior are dependent on system noise. Assuming your Cotamer programs are
also deterministic, their behavior will be dependent only on initial
conditions: a configuration and a single random seed. This will mitigate one
of the biggest problems in distributed systems programming, namely the
difficulty of reproducing a failure. A simulated-system failure under some
random seed should be perfectly replicable by rerunning with the same seed,
and exploring many random seeds should explore a wide range of system
behavior—as long as you built your system right.

Call `cotamer::set_clock(cotamer::clock::real_time)` to change Cotamer to
real-time mode.


## Advanced topics

### Rearming events

Since events are one-shot, a long-lived coroutine that repeatedly waits for
notifications needs a fresh event each time. The `event& event::arm()`
convenience function simplifies this. If `e` has triggered, `e.arm()`
reassigns `e` to a fresh untriggered event; otherwise `e.arm()` leaves `e`
unchanged. In both cases it returns `e`.

Here, a producer enqueues work and calls `trigger()` on a notifier event,
while a background worker uses `work_queue_wakeup.arm()` to ensure that it
always `co_await`s an untriggered event:

```cpp
cot::event work_queue_wakeup;
std::deque<Item> work_queue;

cot::task<> background_worker() {
    while (true) {
        while (work_queue.empty()) {
            co_await work_queue_wakeup.arm();
        }
        auto item = std::move(work_queue.front());
        work_queue.pop_front();
        // ... process item ...
    }
}

void enqueue_work(Item item) {
    work_queue.push_back(std::move(item));
    work_queue_wakeup.trigger();   // does nothing unless background_worker is waiting
}
```

Since `arm()` may reassign the event, it is not thread-safe. It only
modifies `e`—other copies of the original event are unaffected:

```cpp
void test_event_arm_copies() {
    cot::event e;                  // construct untriggered event
    cot::event e_copy = e;         // copy shares same underlying occurrence
    assert(e == e_copy);           // `operator==` tests underlying occurrence
    assert(!e.triggered() && !e_copy.triggered());
    e.trigger();                   // trigger underlying occurrence
    assert(e.triggered() && e_copy.triggered());  // also triggers copy
    e.arm();                       // rearm `e`
    assert(e != e_copy);           // `e_copy` still refers to original
    assert(!e.triggered() && e_copy.triggered());
}
```


### Lazy tasks

A *lazy* task waits to execute until its value is needed. Place
`co_await cotamer::interest{}` at the point where the task should pause:

```cpp
cot::task<int> lazy() {
    co_await cot::interest{};      // pause here until someone wants the result
    co_await cot::after(1h);       // then do the real work
    co_return 42;
}
```

The task runs up to the `interest{}` point and suspends. It resumes when
another task starts waiting for the task with `co_await t`, with `co_await
cot::attempt(t, ...)`, or by calling `t.start()`.

`interest{}` composes with `any()`, allowing a task to auto-start after a
timeout even if nobody has asked for its result:

```cpp
co_await cot::any(cot::interest{}, cot::after(5h));
// resumes on interest OR after 5 hours, whichever comes first
```


### Event construction and introspection

`event()` constructs a new, untriggered event. To construct an
already-triggered event, use `event{nullptr}`. Given an event `e`,
`e.triggered()` returns true if `e`’s underlying occurrence has triggered. You
may also test whether two events refer to the same underlying occurrence with
`e1 == e2`.


### Task construction and introspection

The `t.done()` function call returns true if task `t` has completed.
`t.completion()` returns an event that triggers when the task completes.
Although a task’s return value can be harvested at most once (with `co_await
t`), any number of coroutines can wait for a task to complete (with `co_await
t.completion()`).

Most `cotamer::task` objects are constructed automatically by calling a
coroutine, but default construction with `task<T>()` produces an empty task
with no associated coroutine. A non-empty task becomes empty when it is
detached, when it is destroyed with `t.destroy()`, or when its coroutine is
moved to another task object with `std::move`. Test for task emptiness with
`t.empty()`. Empty tasks are not `done()` and their `completion()` events
never trigger.

Within a coroutine, the expression `co_await cotamer::interest_event{}`
immediately returns an `event` that exposes the `interest{}` state.
Specifically, a coroutine’s `interest_event` event triggers when a different
coroutine starts waiting for the first coroutine’s result. Note, however, that
an `interest_event` cannot trigger until its coroutine suspends for the first
time. (Before that first suspension, the coroutine cannot tell whether its
caller is interested in its result.)
