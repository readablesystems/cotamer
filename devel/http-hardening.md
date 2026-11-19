# HTTP/I/O hardening: status

Security audit and fixes for using cotamer's HTTP and I/O layers in a
single-threaded, internet-facing server (motivating case: porting
`hotcrp-comet` from Tamer).

**State as of 2026-11-19:** all items below are implemented and tested, but
**nothing is committed**. The work is in the working tree on `main` (11
modified files plus the untracked `test/cot-test-http.cc`). Eddie still
intends to review the fixes for items 2–6.


## Audit findings and resolutions

| # | Finding | Resolution |
|:--|:--------|:-----------|
| 1 | `default_status_message()` called `llhttp_status_name()`, which `abort()`s on unknown codes (e.g., 270, 600) | Unknown codes get an empty reason phrase (Eddie's change). Removed bogus 110–113 "Early Hints" table entries (they were `Warning:` codes). `send_response` throws `std::invalid_argument` for codes outside 100–999; `status_code_` widened to `unsigned` so large codes can't wrap into range. |
| 2 | `accept()` threw on EMFILE/ECONNABORTED/etc.; a detached accept loop died silently and permanently | `accept()` retries per-connection errors (ECONNABORTED, EPROTO, ENETDOWN, ENETUNREACH, EHOSTDOWN, EHOSTUNREACH, ENONET, Linux EPERM) and backs off 5ms→1s on EMFILE/ENFILE/ENOBUFS/ENOMEM. Other errors still throw, so closing the listening fd ends the loop. |
| 3 | No CR/LF/NUL validation: header, status-line, and request-line injection | `add_header` throws for empty/non-token names (`us_http_tchar`) and values with CR/LF/NUL (Eddie's `us_header` class). Request targets (also no SP/HTAB) and status messages are checked at send time. Covers the parser's `Host`. |
| 4 | Copy constructor dropped `hpairs_` → duplicate, conflicting `Content-Length` | Copies `hpairs_`. |
| 5 | `clear()` left `headers_` → previous headers (e.g., `Set-Cookie`) leaked into the next message | `do_clear()` clears `headers_`. Note hotcrp-comet uses exactly this reuse pattern (`res_.clear()`). |
| 6 | `send_response_chunk("")` emitted the terminating chunk → response desync | Empty chunks are ignored. |
| 7 | No limits: unbounded headers/body, `Content-Length` passed straight to `reserve()` (throwing `bad_alloc` through llhttp's C frames), no timeouts | `http_parser::set_max_header_size` (default 64 KiB), `set_max_body_size` (16 MiB), `set_receive_timeout` (60s server, none client; `duration::max()` disables). Failures give `HPE_USER` plus `failure()` = `header_too_large` / `body_too_large` / `timeout`, and `error_reason()` text. Oversized `Content-Length` is rejected before body bytes arrive. Body pre-reservation capped at 1 MiB with geometric growth. |
| 8 | Upgrade/CONNECT: a phantom empty request, then tunneled bytes parsed as new requests; no way to detect upgrades | `http_message::upgrade()`. After an upgrade message, `should_keep_alive()` is false, later `receive()` fails with `HPE_PAUSED_UPGRADE`, trailing bytes stay in `receive_buffer()`. |
| 9 | Forwarding a parsed message keeps `Transfer-Encoding: chunked` though the body is de-chunked (proxy smuggling) | **Not implemented** (server doesn't proxy). Proposal below. |
| 10 | Library-created sockets weren't close-on-exec | `detail::nonblocking_socket` / `detail::nonblocking_accept` in `io.hh`: `SOCK_NONBLOCK\|SOCK_CLOEXEC` + `accept4` where available (Linux); `fcntl` right after creation on macOS (racy only against a concurrent fork+exec in another thread; commented). Used by accept, tcp/udp listen/connect, curl's opensocket callback. |
| 11 | curl: redirects to any protocol (`file://`), unlimited redirects, no connect timeout; `headers` merged across redirect hops | HTTP/HTTPS only (also for redirects), `MAXREDIRS` 10, `CONNECTTIMEOUT` 30s; headers cleared at each status line so only the final response's remain. hotcrp-comet doesn't use curl. |
| 12 | Responses without a body had no `Content-Length` (client hangs); HEAD responses sent bodies | `send_response` adds `Content-Length` when no CL/TE header (including `0`); never sends a body for 1xx/204/304; HEAD responses send headers only (CL = body length if nonempty). |
| 12b | Client parser didn't know HEAD responses have no body, or that 2xx CONNECT starts a tunnel | `send_request` records methods (`pending_methods_`, also used server-side for HEAD); `on_headers_complete` returns 1 (no body) for HEAD, 2 (upgrade) for 2xx CONNECT. Body limit doesn't apply to HEAD responses. |
| 13 | Cancelling `receive()` mid-message left the parser mid-parse; the next `receive()` returned a truncated message | `receive_guard`: cancellation (or an exception) mid-message fails the parser with `failure_type::cancelled`; cancellation between messages leaves it usable. A failed parser's `receive()` returns immediately. |
| 14 | `path()` not normalized | Kept raw. New opt-in `decoded_path()` → `std::optional<std::string>`: percent-decodes, resolves `.`/`..`, rejects `%00`, `%2F`, invalid escapes, and escaping above `/`. |

Smaller fixes made along the way:
- `should_keep_alive()` returns false whenever `!ok()`.
- `make_info` never set `info_url`, so the URL was re-split on every `path()`.
- Query-string `%` decoding bounds check was off by one (`s2 - s >= 2` → `>= 3`; harmless before because of the NUL terminator).
- `accept()` and the socket helpers no longer leak an fd when setting flags fails.


## Behavior notes and caveats

- **Virtual time.** With the virtual clock, the 60s server receive timeout
  fires almost immediately when the peer is an external process (the loop
  jumps to the next timer when no fd is ready). In-process peers are fine.
  All HTTP servers in the repo use real time.
- **EMFILE on macOS** drops the pending connection (the kernel dequeues it
  before allocating the fd); on Linux it stays queued. `test_accept_fd_exhaustion`
  only checks that connections made after recovery are accepted.
- **Upgrades** close the connection unless the app takes over the fd; an app
  can't decline an `Upgrade:` request and keep parsing HTTP. Fine for now.
- **1xx responses with client pipelining**: tickets assume one response per
  request, so an interim 100 Continue would be handed to the wrong ticket.
  Pre-existing; not addressed.


## #9 proposal (not implemented)

1. Add `http_message::remove_header(name)`, so a proxy can strip hop-by-hop
   headers (`Connection`, `Keep-Alive`, `Transfer-Encoding`, `TE`, `Upgrade`,
   `Proxy-*`).
2. In the send functions, treat a nonempty in-memory body as authoritative:
   drop any `Transfer-Encoding`/`Content-Length` and send the correct
   `Content-Length`. Honor `Transfer-Encoding: chunked` only with an empty
   body (streaming via `send_response_chunk`). This also catches a
   user-supplied `Content-Length` that doesn't match the body.


## Files

- `cotamer/http.hh`, `cotamer/http.cc`: items 1, 3–8, 12–14.
- `cotamer/io.hh`, `cotamer/io.cc`: items 2, 10.
- `cotamer/curl.hh`, `cotamer/curl.cc`: items 10, 11.
- `COTAMERIO.md`: documents accept retries, close-on-exec, header validation,
  `decoded_path()`, `upgrade()`, response framing, client HEAD/CONNECT,
  limits and timeouts (with a 413 example).
- `CMakeLists.txt`: new `cot-test-http` target (when llhttp is found).
- `GNUmakefile`: `cot-test-http` in `targets`; `make check` builds and runs it
  if `config.hh` has `COTAMER_HAVE_LLHTTP 1`.


## Tests

New `test/cot-test-http.cc` (untracked; remember to `git add`):
`unknown_status_code`, `header_injection`, `copy_keeps_headers`,
`clear_removes_headers`, `empty_chunk`, `invalid_status_code`, `size_limits`,
`receive_timeout`, `receive_cancel`, `upgrade`, `response_framing`,
`client_head`, `client_connect`, `decoded_path`. The bug-fix tests (first five,
plus `client_head`/`client_connect`) were confirmed to fail against the
pre-fix code.

Additions elsewhere:
- `test/cot-test-io.cc`: `accept_fd_exhaustion` (lowers `RLIMIT_NOFILE`;
  fails on the old code), `sockets_cloexec`.
- `test/cot-test-curl.cc`: `redirect` now checks only final-response headers;
  new `protocols` (rejects `file://`, directly and via redirect).

Last results: `make check` passes on macOS; `cot-test-http`, `cot-test-io`,
`cot-test-curl`, `cot-test`, `cot-manual` all pass on Linux (GCC 15.2,
libcurl 8.14.1, llhttp present) with no new warnings.


## Running tests

macOS: `make check`; curl tests separately with
`make cot-test-curl && build/cot-test-curl`.

Linux, via the `cs2620` image (non-interactive; builds in the container's
`/tmp` so nothing lands in the repo):

```sh
docker run --rm -v "$PWD":/home/cs61-user/cs2620 -w /home/cs61-user/cs2620 \
    cs2620:latest bash -c 'cmake -B /tmp/b && cmake --build /tmp/b -j8 \
    && for t in cot-test-http cot-test-io cot-test-curl cot-test cot-manual; \
       do /tmp/b/$t >/dev/null 2>&1; echo "$t exit=$?"; done'
```

Environment issues seen on the old iMac:
- `build/CMakeCache.txt` pointed curl at the old `/Applications/Xcode.app`
  SDK, so `cot-test-curl` failed to compile there; a fresh build directory
  works.
- One `-j8` Linux build hit a GCC 15.2 internal compiler error at
  `cotamer_impl.hh:320` (unchanged code); four later builds were clean.
  Looks like an intermittent GCC bug.


## Possible next steps

- Eddie's review of items 2–6; then commit (probably as several commits by
  topic: HTTP message fixes, accept/cloexec, limits/timeouts/cancel, upgrade
  and framing, curl, decoded_path).
- #9 if proxying is ever needed.
- When porting hotcrp-comet: call `set_receive_timeout()` before each
  `receive()` to mirror its `connection_timeout` / 5s keep-alive timeouts;
  respond 413/431 on `failure()` if desired. It routes on raw path suffixes,
  so it doesn't need `decoded_path()`.
