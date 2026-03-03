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

A task begins running as soon as it is called, and keeps running until it hits a
`co_await`. Then it suspends and other tasks get a chance to run. When its
awaited event or task becomes ready, it resumes where it left off.

The `main` function starts `main_task`, which starts `slow_add`. Both
`slow_add` and `main_task` then suspend themselves, `slow_add` waiting on a
timer and `main_task` waiting on `slow_add`. `cotamer::loop()` then advances
virtual time and runs unblocked tasks until there’s nothing left to do.


## Events

The key benefit of coroutine programming is that functions can suspend
themselves while waiting for external occurrences. In Cotamer, the **event**
abstraction represents these occurrences.

Events start in the **untriggered** state and then transition to the
**triggered** state. Once triggered, an event stays triggered forever: events
are one-shot notifications. A task can wait for an event with `co_await e`.

Cotamer functions can create events associated with specific future occurrences:

| Function                   | Returns an event that triggers...           |
|:---------------------------|:--------------------------------------------|
| `cotamer::after(duration)` | after the given duration elapses            |
| `cotamer::at(time_point)`  | at the given absolute time                  |
| `cotamer::asap()`          | as soon as possible (next loop iteration)   |

A full event-driven system would offer other primitive events, such as
`readable(fd)` (triggers once file descriptor `fd` becomes readable), and as
class progresses we will add those too. But in the context of a deterministic
network simulation, time is the only primitive event.

An event can be also created explicitly with `cotamer::event e` and triggered
with `e.trigger()`. The `e.triggered()` function tests whether an event has
triggered yet.


## Combinators: `any`, `all`, and `attempt`

`cotamer::any()` and `cotamer::all()` combine multiple events into one; you
can supply as many events as you like.

```cpp
// Resume after 1 hour (whichever event fires first)
co_await cot::any(cot::after(1h), cot::after(10h));

// Resume after 10 hours (wait for both)
co_await cot::all(cot::after(1h), cot::after(10h));
```

`cotamer::attempt(task, event...)` runs a task with cancellation. The `task`
is cancelled if any of the `event`s trigger before the task completes.
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

Coroutine data, such as local variables, are normally tied to the lifetime of
the `task<>` object. If the `task<>` is destroyed, then all data associated
with the task is destroyed too.

```cpp
cot::task<> printer(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::asap();
    std::print("printer({}) completed\n", i);
}

int main() {
    printer(0);             // returned `task` destroyed → coroutine destroyed
    auto t = printer(1);    // returned `task` preserved → coroutine lives
    cot::loop();
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(1) completed
```

If you don’t want this, the `task::detach()` function detaches a task,
allowing it to run to completion even if no one is interested in its result.
The task’s memory will be automatically freed when the task returns.

```cpp
cot::task<> printer(int i) {
    std::print("printer({}) began\n", i);
    co_await cot::asap();
    std::print("printer({}) completed\n", i);
}

int main() {
    printer(0).detach();    // returned `task` detached → coroutine lives
    auto t = printer(1);    // returned `task` preserved → coroutine lives
    cot::loop();
}

// Outputs:
// printer(0) began
// printer(1) began
// printer(0) completed
// printer(1) completed
```

The `cotamer::loop()` function runs until there’s nothing left to do—no
outstanding `asap` events, no coroutines, no timers. To cause this loop to
exit early, call `cotamer::clear()`, which clears all these task sources and
destroys outstanding coroutines.


## Deterministic virtual time

Cotamer uses **deterministic virtual time**. The system starts running at a
fixed timestamp, and system time appears to jump forward whenever the system
would block. For instance, this program completes instantaneously, even though
it reports that over a year has elapsed:

```cpp
#include "cotamer.hh"
#include <print>
namespace cot = cotamer;
using namespace std::chrono_literals;

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


## Advanced topics

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


### Introspection

Tasks and events offer functions to introspect their state. `e.triggered()`
returns true if event `e` has triggered; `t.done()` returns true if task `t`
has completed. You can also obtain an event that triggers when a task
completes. This event, `t.completion()`, can be useful because multiple
coroutines can wait on it (at most one coroutine can wait on the task itself).

A coroutine has no access to its `task<T>` object, so task self-introspection
takes some work. Within a coroutine, the expression `co_await
cotamer::interest_event{}` returns an `event` that triggers once another task
is interested in this task’s result. You can use that event’s `triggered()`
function to test for interest. Note that `co_await cotamer::interest_event{}`
never suspends the current task (unlike `co_await cotamer::interest{}`).
