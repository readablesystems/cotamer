# Cotamer audit — 2026-11-20

Findings from a review of `cotamer/` on `main` (after the `stream` port).
Each item has a stable number; refer to it in commits (e.g., "audit #12").

Legend: **[x]** fixed · **[ ]** open.
Severity: **bug** (reachable today) · **latent** (no current caller, or only
on unusual platforms/paths) · **nit** (docs, comments, consistency).
Verification: *repro* = reproduced with a scratch program;
*read* = confirmed by reading the code only.

Line numbers are approximate as of this audit.


## Core runtime (`cotamer.hh`, `cotamer_impl.hh`, `cotamer.cc`, …)

### 1. [ ] bug — `fd_body::deref` race leaks fd and body
`cotamer_impl.hh:124`. `deref()` does `if (ref_.load() == 1) close(true);
else ref_.fetch_sub(1);`. Two threads dropping the last two references can
both load 2 and both `fetch_sub`, so `close(true)` never runs: the OS fd and
the `fd_body` leak. Stress test with two threads dropping copies
simultaneously leaked 19,881 / 20,000 fds. *repro*

Fix: decide with `ref_.fetch_sub(1, std::memory_order_acq_rel) == 1`, then
close/delete without decrementing again inside `close`.

### 2. [x] bug — `loop()` exits with pending `asap()` work
`cotamer.cc:116`. The exit test checks timers, fds, lock, guards, and
keepalives, but not `asap_`. With more than `asap_quota` (4096) queued asap
events, `loop()` returns while coroutines are still runnable.
`driver::empty()` (`cotamer_impl.hh:1328`) has the same gap; `~driver` does
check `asap_`. Repro: one coroutine doing `for (i < 5000) co_await
cot::asap();` reaches 4096 after the first `loop()`. *repro*

Fixed: added `asap_.empty()` to both. Test `asap_over_quota`.

### 3. [x] bug — virtual time can go backwards
`cotamer.cc:154`. `snow_ = timed_.top_time()` runs even when
`top_time < snow_`, which happens when an asap batch already advanced time
via `step_time()` past a near timer. Repro: one task awaits
`after(duration{1})`, ten tasks await `asap()`; the timer task observes
`steady_now()` 8 ticks earlier than the asap tasks did. *repro*

Fixed: `snow_ = std::max(snow_, timed_.top_time())`. Required by the #12
fix, which puts past deadlines in the timer heap in virtual time (so
`at(now - 1h)` would otherwise move the clock back an hour). Test
`time_never_backwards` (fails on the old code).

### 4. [ ] bug — `co_await interest{}` hangs when task is already awaited
`cotamer_impl.hh:1036`. `start()` sets `has_interest_ = true` when
`interest_` is null, but `prepare_awaiter` only triggers `interest_` if it
already exists. Repro: `lazy() { co_await after(1s); co_await interest{};
co_return 42; }` with the caller doing `co_await lazy()` — caller never
resumes; `loop()` exits. COTAMER.md says awaiting a task starts it. *repro*

Fix: in `prepare_awaiter`, `else has_interest_ = true;` (mirror `start()`).

### 5. [x] bug — `driver_guard` outliving `clear()` drives `guard_count_` to −1
`cotamer_impl.hh:1425`, `cotamer.cc:197`. `process_clearing` zeroes
`guard_count_`; `clearing_` resets when the loop exits; a guard destroyed
afterwards decrements to −1. Repro: `{ driver_guard g; clear(); loop(); }`
— afterwards `driver::empty()` is false and later `loop()`s never exit.
*repro*

Fixed: `process_clearing` bumps a clear generation; a guard records the
generation it was counted in and decrements only if it is unchanged.
(Clamping at 0 would let a pre-clear guard release a post-clear guard's
count.) Test `guard_outlives_clear`.

### 6. [x] latent — `any()`/`all()` don't accept awaitables
`cotamer.hh:151`, `cotamer_impl.hh:393`. The header comment said they do,
but the catch-all `make_event(T resumable)` tried to be a coroutine
returning `event` (no `promise_type`), so any argument that reached it
failed to compile with “cannot be a coroutine.” That included
`const event&` (no matching overload), so `any(const_ev, …)` and
`attempt(t, const_ev)` failed. *repro*

Fixed (option 1 of 3): `any`/`all`/`attempt` take events (and types
convertible to `event`), `mutex_event`s, and `interest{}` only. Added
`make_event(const event&)`; replaced the catch-all with a constrained
template whose `static_assert` points users at `first`/`race`/`attempt`
for tasks; corrected the header comment. Test `any_const_event`.
Rejected: accepting borrowed tasks via `resolution()` (fires at
`resolve{}` points, not just completion) and wrapping arbitrary
awaitables in detached helper coroutines (uncancellable, can dangle).

### 7. [x] latent — `co_await cot::first()` (no args) crashes
`cotamer_impl.hh:1773`. Returns an empty `task<>`; `task_awaiter::await_ready`
calls `.done()` on a null handle. Zero-arg `race<T>()` blocks forever
instead. *repro*

Fixed: `first()` now never completes, like `race<T>()` and `any()`. Test
`first_zero`.

### 8. [ ] latent — `set_user_flags` ignores `ef_lock`
`cotamer_impl.hh:448`. The CAS loop can succeed while another thread holds
the event lock; the holder's later `unlock(flags)` overwrites the user-flag
update with stale flags. Matters only cross-thread (e.g.,
`pop_watch_list_event` setting flags on an event another driver is
locking). *read*

Fix: spin while `ef_lock` is set, as `lock()` does.

### 9. [ ] latent — moving a `unique_lock`/`shared_lock` while its request is pending
`cotamer_impl.hh:2219, 2294`. `m_->lock(owned_)` stores `&owned_` in the
waiter list. If the lock object moves before the grant, the grant sets the
moved-from object's `owned_ = true` while its `m_` is null, so its
destructor calls `nullptr->unlock()`, and the new owner never unlocks. *read*

Fix: document “must not be moved while pending,” or notify the owner
indirectly.

### 10. [ ] latent — `small_vector::push_back(const T&)` with aliasing argument
`small_vector.hh:116`. On reallocation, `push_space()` destroys/frees the
source before `construct_at`. No current caller does this. *read*

Fix: copy into a temporary first when `sz_ == cap_`.

### 11. [x] nit — single-argument `race(t)` returns `task<T>&`
`cotamer_impl.hh:1803`. Declared return is `task<common...>`, so
`auto r = race(t);` doesn't compile while `race(t1, t2)` does.

By design: `race(x)` is the identity, matching `race(task<T>&&)`, with no
wrapper coroutine. `auto r = race(t)` fails exactly as `auto r = t` does.
Documented in the `race` comment in `cotamer_impl.hh` (kept out of the
header); test `race_single_lvalue`.

### 12. [x] bug — `at()`/`after()` behave differently in virtual time
`driver::at(t)` returned an already-triggered event when `t <= now` in
virtual time, but went through the timer heap in real time; `at(t, e)` and
`after(d, e)` always used the heap. Since awaiting a triggered event doesn't
suspend, `co_await after(0ms)` yielded to other tasks in real time but not
in virtual time: `while (!flag) co_await after(0ms);` waiting on another
task ran once in real time and spun forever in virtual time. *repro*

Fixed: removed the fast path, so timers never trigger immediately, even
with a past deadline; the driver loop triggers them, on both clocks.
Documented above `driver::at` in `cotamer_impl.hh`. Depends on the #3 fix.
Tests `at_past_deadline` and `after_zero_yields`.

### 13. [x] nit — `lf_quorum` comment says “bit 1”
`cotamer_impl.hh:539`. `lf_quorum = 1` is bit 0. Comment fixed.

### 14. [x] nit — `timer_heap::order_` comment says “next `order` to insert”
`timer_heap.hh:56`. `emplace` uses `++order_`, so it holds the most recent
order. Comment fixed.

### 15. [x] latent — `mutex_event::trigger()` declared, never defined
`cotamer.hh:491`. Unused; declaration removed.

### 16. [x] latent — `operator!(fdevent)` inverted
`cotamer.hh:39` returned `e != fdevent::none`. Fixed; test added in
`stream_basics`.


## I/O (`io.hh`, `io.cc`, `curl.*`, fd parts of the driver)

### 17. [x] bug — `sendv` declaration doesn't match definition
`cotamer.hh:388` declared `sendv(fd, const void* buf, size_t count, int
flags = 0)`; `io.hh:197` defines `sendv(fd, const iovec*, size_t, int)` with
no default. `cot::sendv(f, iov, n)` bound to the undefined overload →
`-Wundefined-inline` and a link error. *repro* Declaration fixed.

### 18. [ ] bug — `closed(f)` busy-spins on kqueue; livelocks in virtual time
`io.cc:260` (`fd_batch::add`). A close-only mask (4) intersects both
`read_close` and `write_close`, so it registers level-triggered `EVFILT_READ`
*and* `EVFILT_WRITE`. When the socket is writable, WRITE fires with
`mask_in = write`; `take_watch_list` finds no matching watch and changes
nothing, so the next `kevent` returns immediately again. Socketpair test,
peer closes after 500ms: real time burns 0.45s CPU in 0.5s wall; virtual
time hangs forever because `had_fd_event` stays true and `snow_` never
advances (`cotamer.cc:149`). `any(readable(f), closed(f))` has the same
problem while no data is pending. epoll is fine (close → `EPOLLRDHUP`).
*repro*

Fix: don't add `EVFILT_WRITE` for close-only interest; make close-only READ
fire only on EOF (`EV_CLEAR`, or a huge `NOTE_LOWAT`).

### 19. [ ] bug — `accept` throws on transient errors
`io.hh:305`. Only `EAGAIN`/`EWOULDBLOCK`/`EINTR` are retried.
`ECONNABORTED` (client RST before accept, macOS/BSD) throws and kills
`tcp_accept` loops (e.g., `examples/httpd.cc`, the COTAMERIO echo server).
Linux `accept(2)` also says to treat `ENETDOWN`, `EPROTO`, `ENOPROTOOPT`,
`EHOSTDOWN`, `ENONET`, `EHOSTUNREACH`, `EOPNOTSUPP`, `ENETUNREACH` like
`EAGAIN`. *read*

### 20. [ ] bug — curl response headers accumulate across redirects
`curl.cc:272`. With `FOLLOWLOCATION`, the header callback skips `HTTP/`
status lines but never resets `headers`. A 302→200 yields both
`Content-Length` values plus `Location` and redirect-only headers. *repro*

Fix: `cr->resp.headers.clear()` on a line starting with `HTTP/` (also
handles 1xx).

### 21. [ ] latent — `COTAMER_USE_POLL` fallback broken
`io.cc:413`: `poll(...)` inside a `driver` member resolves to
`driver::poll()` — doesn't compile; needs `::poll`. `io.cc:414`:
`batch.size = batch.ev.size()` ignores `poll`'s return, so `watch_fds`
always reports activity and virtual time never advances while any fd is
watched; poll errors/`EINTR` are ignored. *repro*

### 22. [ ] latent — `fd_event_set::watch` segfaults on stale body
`io.cc:133`. When the fd number was closed and reused behind cotamer's
back, `fd_close(fd)` returns `nullopt` (the old body is still open), and
`fx->first` dereferences an empty optional. Repro: `fd a(p[0]);
readable(a); ::close(p[0]); pipe(); fd b(reused); readable(b)` → SIGSEGV.
*repro*

Fix: if `fx`, `remove_listener`; otherwise detach the old body manually
(trigger watches, bump epoch, `remove_listener`).

### 23. [ ] latent (Linux) — UDP listeners can silently share a port
`io.cc:653` sets `SO_REUSEADDR` on UDP sockets. On Linux that allows
multiple binds to the same addr:port; two `udp_listen("…:9000")` both
succeed and datagrams are split. (BSD/macOS: second bind fails.) *read*

Fix: don't set `SO_REUSEADDR` for UDP.

### 24. [ ] latent (Linux) — `":PORT"` may listen on IPv4 only
`io.cc:546`. With `address_any`, the IPv6 socket is made dual-stack, which
only helps if `::` comes first. macOS returns `::` first (confirmed); glibc
likely returns `0.0.0.0` first, so `tcp_listen(":P")` would be IPv4-only and
in `tcp_listen_all` the later `::` bind fails `EADDRINUSE` and is dropped.
*read, medium confidence*

Fix: when `address_any`, try `AF_INET6` results first.

### 25. [ ] latent (Linux) — `readable()` on a regular file throws from `loop()`
`epoll_ctl(ADD)` on a regular file/directory fails with `EPERM`;
`io.cc:281` throws from inside `driver::loop` and the waiter never wakes.
kqueue accepts such fds. *read*

Fix: on `EPERM`, trigger watches immediately (regular files are always
ready).

### 26. [ ] latent — raw fd leaks if socket setup throws
If `set_nonblocking`/`set_no_sigpipe` throws, the raw fd leaks in `accept`
(`io.hh:300`) and in `tcp_listen_all`/`tcp_connect`/`udp_*` (`io.cc:540,
601, 648, 708`). `curl.cc` already handles this correctly. *read*

### 27. [ ] nit — epoll sub-millisecond busy-poll
`io.cc:23`. `duration_milliseconds` truncates, so a timer 0.9ms away
busy-polls with `epoll_wait(…, 0)`. Round up.

### 28. [ ] nit — waiting on a closed `fd` never fires
`io.cc:118`: `watch` with `fd < 0` returns a never-triggering event, so
`co_await closed(f)` / `readable(f)` after `f.close()` hangs. This is the
documented rule, but `empty_stream` now fires immediately, so a
`file_stream` over a closed fd behaves differently from `empty_stream`.
Decide whether `closed(f)` on a closed fd should fire.

### 29. [x] nit — `tcp_accept` copies `listen_fd`
`io.hh:317`. Now `std::move`s it into `accept`.

### 30. [ ] nit — COTAMERIO.md errors
- line 89: `cot::ioresult result = cot::read(fd, buf, n);` missing
  `co_await`.
- line 195: DNS failures are documented as `std::runtime_error`; the code
  throws `std::system_error` with `getaddrinfo_error_category`.
- line 58: list of functions returning non-blocking fds omits
  `udp_listen`/`udp_connect`.
- socket table omits `sendv` and the `flags` parameters.

Not a bug (rejected): `sendv_all` passing `MSG_WAITALL` to a send.
`sendmsg` interprets it as “send everything” and strips it before the
system call (`io.cc:792`).


## HTTP (`http.hh`, `http.cc`)

### 31. [ ] bug — dropping a `send_request` ticket deadlocks `receive()`
`http.cc:592, 637`. When the read mutex is free, `m_[0].lock()` acquires it
immediately; `mutex_event` has a defaulted destructor, so destroying the
ticket never releases it. `http_parser::send()` on a client always drops the
ticket, so `co_await hp.send(req); co_await hp.receive();` hangs forever, as
does ignoring `send_request`'s result. COTAMERIO.md (“Pipelining”) says
“If you aren't using pipelining, you can ignore the ticket.” *repro*

Fix: have a parser-owned “next ticket” slot that `receive()` uses when called
without one (and `send()` stores into), or make an unclaimed, acquired
`mutex_event` unlock on destruction. Fix the docs either way.

### 32. [ ] bug — `http_message` copy constructor doesn't copy `hpairs_`
`http.hh:309`. The copy keeps `headers_` but loses header offsets:
`m.has_header("content-type") == 1`, `c.has_header(...) == 0`, iteration
empty, but `send_*` still writes the raw headers. Copy assignment does copy
`hpairs_`. *repro*

Fix: add `hpairs_(x.hpairs_)`.

### 33. [ ] bug — `http_message::clear()` doesn't clear `headers_`
`http.cc:201` (`do_clear`). `hpairs_` is cleared but `headers_` is not:
`m.header("X-Old","1"); m.clear(); m.url("/x").header("X-New","2")` sends
both headers. `examples/httpd.cc` only survives `res.clear()` because `res`
was just moved from. *repro*

Fix: `headers_.clear();`.

### 34. [ ] bug — unknown status code aborts the process
`http.cc:159`. The fallback calls `llhttp_status_name`, which `abort()`s on
unknown codes: `default_status_message(777)` → exit 134. Known llhttp codes
not in the local table return enum identifiers (299 →
`"MISCELLANEOUS_PERSISTENT_WARNING"`). *repro*

Fix: return `""` (or `"Unknown"`) instead of calling llhttp.

### 35. [ ] bug — huge `Content-Length`/chunk size → unbounded `reserve`
`http.cc:481, 489`. `reserve(hp->content_length)` with
`Content-Length: 99999999999999999` throws `bad_alloc` through llhttp's C
frames; 4000000000 reserves 4 GB per request. Remotely triggerable. *repro*

Fix: cap the reservation (e.g., `min(content_length, 1 MiB)`) or drop it.

### 36. [ ] bug — no `llhttp_finish()` at EOF
`http.cc:522, 561`. A close-delimited response (no Content-Length, not
chunked) never completes: `"HTTP/1.0 200 OK\r\n\r\nhello"` + EOF returns
`HPE_CLOSED_CONNECTION` even though `body == "hello"`. *repro*

Fix: on EOF call `llhttp_finish(&hp_)`; if it returns `HPE_PAUSED`
(`on_message_complete` ran), treat the message as complete.

### 37. [ ] bug — bodiless responses have no framing
`http.cc:617`. `send_response(http_message(404))` emits
`HTTP/1.1 404 Not Found\r\n\r\n` with no `Content-Length`, so the peer must
read to EOF while the server keeps the connection alive. Combined with #36,
the client then errors. *repro*

Fix: without `Transfer-Encoding`, add `Content-Length: 0` unless status is
1xx/204/304.

### 38. [ ] bug — `receive()` after a parse error reads stale `error_pos`
`http.cc:533`. Once llhttp is in an error state, `llhttp_execute` returns
immediately without updating `error_pos`; the stale pointer (into an old
buffer or coroutine frame) yields a garbage `consumed`, and
`std::string(buf + consumed, buf + nr)` throws `length_error` (UB). Repro:
`"GET / …\r\n\r\nBOGUS\x01 / …"` and three `receive()` calls. *repro*

Fix: if `hp_.error` is set (and not a pause) on entry, return it
immediately; only use `error_pos` for errors produced by this call.

### 39. [x] latent — `has_path()` declared, never defined
`http.hh:124`. Defined as `has_url_field(0)`.

### 40. [ ] latent — `send_response_chunk("")` terminates the body
`http.cc:646`. Writes `0\r\n\r\n`, the end marker; later chunks corrupt the
stream. *repro*

Fix: return early on empty `str`.

### 41. [ ] latent — `receive()` after an upgrade returns a fake success
`http.cc:546`. After an `Upgrade:` request, a second `receive()` hits
`HPE_PAUSED_UPGRADE` and returns `HPE_OK` with an empty URL and stale
method. Also, `upgrade_` has no public getter. *repro*

Fix: return an error once upgraded; add an `upgrade()` accessor.

### 42. [ ] latent — `%` bounds check off by one in query decoding
`http.cc:308`. Checks `s2 - s >= 2` but reads `s[2]`; should be `>= 3`.
Unreachable today because `s2` always points at `'#'` or the terminator.

### 43. [ ] latent — forwarding a parsed message can break framing
The parser never sets `has_body_`, and a de-chunked `body_` is sent with the
original `Transfer-Encoding: chunked` header. Matters only for proxy-style
reuse.

### 44. [ ] nit — `add_header` doesn't validate names/values
`us_http_tchar` is computed but unused. A value containing CR/LF allows
header injection.

### 45. [ ] nit — `make_info` never sets `info_url`
`http.cc:234`. Every `path()`/`search()`/`hash()` reparses the URL. Correct
results, wasted work.

### 46. [ ] nit — dead/undone status initialization
`http.cc:426`: in `on_status`, `major_ == 0` is never true because `clear()`
sets it to 1. The same `clear()` in `on_message_begin` undoes
`md.hm.status_code(0)` from `http.cc:512`.

### 47. [ ] nit — received responses report a bogus method
`copy_parser_status` copies `hp_.method` into responses, so
`method_name()` reports `"DELETE"` (method 0).

### 48. [ ] nit — status table maps 110–113 to “Early Hints”
`http.cc:15`. Those are obsolete Warning codes, not status codes.
