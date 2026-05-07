#pragma once
#include "cotamer/cotamer.hh"
#include "cotamer/config.hh"
#include "cotamer/http.hh"
#include <wslay/wslay.h>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

// cotamer/websocket.hh
//    Coroutine-driven WebSocket (RFC 6455) streams built on wslay.
//
//    `ws_stream` holds the per-session state (wslay context, deflate streams,
//    inbound/outbound buffers). It is *not* templated on a transport: the
//    transport (typically `cotamer::fd` for ws://, or `cotamer::tls_stream`
//    for wss://) is passed to each I/O method, the same way `http_parser`
//    takes a stream argument. By convention you'll pass the same transport
//    on every call for a given `ws_stream`; the API doesn't enforce that.
//
//    Server-side use:
//        cot::http_parser hp(cot::http_parser::server);
//        auto req = co_await hp.receive(cfd);
//        if (cot::is_ws_upgrade_request(req)) {
//            auto ws = co_await cot::ws_upgrade(hp, cfd, req);
//            // ... ws.send_text(cfd, "..."), co_await ws.receive(cfd), ...
//        }
//
//    Client-side use:
//        auto cfd = co_await cot::tcp_connect("host:port");
//        auto ws = cot::ws_stream::make_client("host", "/path");
//        co_await ws.handshake(cfd);
//        // ... use cfd with ws methods ...

namespace cotamer {

enum class ws_opcode : uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA
};

// Bitflag options for the ws_stream factory and upgrade functions. The
// takeover-flag names match the RFC 7692 wire parameters directly, so they
// describe the *endpoint identity* (server or client), not "this side" /
// "the other side".
//
//   * `permessage_deflate`: on `make_client`, advertise the extension; on
//     `ws_upgrade`, accept it if the client offers; on `make_server`,
//     indicates the upgrade negotiation enabled it. (RFC 7692.)
//   * `server_no_context_takeover`: the server endpoint resets its deflate
//     dictionary between messages.
//   * `client_no_context_takeover`: the client endpoint resets its deflate
//     dictionary between messages.
enum class ws_options : unsigned {
    none                       = 0,
    permessage_deflate         = 1u << 0,
    server_no_context_takeover = 1u << 1,
    client_no_context_takeover = 1u << 2,
};
constexpr ws_options operator|(ws_options a, ws_options b) noexcept {
    return ws_options(unsigned(a) | unsigned(b));
}
constexpr ws_options operator&(ws_options a, ws_options b) noexcept {
    return ws_options(unsigned(a) & unsigned(b));
}
constexpr ws_options operator~(ws_options a) noexcept {
    return ws_options(~unsigned(a));
}
constexpr ws_options& operator|=(ws_options& a, ws_options b) noexcept {
    a = a | b; return a;
}
constexpr ws_options& operator&=(ws_options& a, ws_options b) noexcept {
    a = a & b; return a;
}
constexpr bool operator!(ws_options a) noexcept {
    return unsigned(a) == 0;
}

// A complete WebSocket data message (text or binary). Fragments are
// reassembled internally by `receive()`.
struct ws_message {
    ws_opcode opcode = ws_opcode::text;
    std::string payload;
};

// Reported by `receive()` when the peer closes the connection (or the
// transport reaches EOF without a close frame, in which case `code` is 1006).
struct ws_close {
    uint16_t code = 0;
    std::string reason;
};

// Category for wslay error codes. `std::error_code::value()` carries the
// original wslay code (negative — `WSLAY_ERR_*` values are negative).
const std::error_category& ws_error_category() noexcept;

// Thrown on protocol or callback errors. `e.code().value()` is the wslay
// error code (negative); `e.code().category() == ws_error_category()`.
class ws_error : public std::system_error {
public:
    explicit ws_error(int wslay_err);
    ws_error(int wslay_err, const std::string& what);
};


namespace detail {
struct ws_state;

// State-machine helpers (defined in websocket.cc). The templated public
// methods of ws_stream call these to drive wslay, then handle transport I/O
// directly. Keeping these non-template lets the heavy code (wslay glue,
// permessage-deflate, sha1, base64) live in a single TU.

void ws_state_init(ws_state*, bool is_client);
void ws_state_set_residual(ws_state*, std::string_view);
void ws_state_enable_deflate(ws_state*, ws_options takeover_flags);

// Returns wslay's want-write status; if there's queued output, hands the
// bytes to `out` (cleared first). Caller writes `out` to the transport.
// Returns true if the loop should continue (more wslay work to do).
bool ws_state_pump_writes_step(ws_state*, std::string& out);

// If buffered recv bytes are present, drive wslay_event_recv on them and
// return true. Otherwise return false and the caller should read from the
// transport and call ws_state_feed_recv.
bool ws_state_drain_recv_buffer(ws_state*);
void ws_state_feed_recv(ws_state*, const void* data, size_t n);
void ws_state_set_recv_eof(ws_state*);

// Try to pop the next completed message off the queue. Returns true on
// success and stores into `out`.
bool ws_state_pop_message(ws_state*,
                          std::variant<ws_message, ws_close>& out);

// Build a `ws_close` reflecting the current shutdown state — used when
// receive() runs out of inputs without a close frame on the queue.
ws_close ws_state_make_close(ws_state*);

bool ws_state_read_done(ws_state*);   // read disabled OR recv_eof
bool ws_state_close_received(ws_state*);
bool ws_state_close_sent(ws_state*);
bool ws_state_is_open(ws_state*);
bool ws_state_deflate_negotiated(ws_state*);

// Queue a frame. Throws ws_error on failure. `queue_close` swallows
// "no more messages" (a benign close-already-queued case) per RFC 6455.
void ws_state_queue_text(ws_state*, std::string_view payload);
void ws_state_queue_binary(ws_state*, const void* data, size_t n);
void ws_state_queue_ping(ws_state*, std::string_view payload);
void ws_state_queue_pong(ws_state*, std::string_view payload);
void ws_state_queue_close(ws_state*, uint16_t code, std::string_view reason);

// True if `state` is a non-null client-mode handle (precondition for the
// client handshake). Defined in cc since it touches `ws_state` internals.
bool ws_state_is_client(ws_state* state);

// Build the HTTP/1.1 GET request line+headers for a client-side handshake.
// `key` is the random Sec-WebSocket-Key the caller will validate against
// the server's accept.
std::string ws_build_client_request(std::string_view host,
                                    std::string_view path,
                                    std::string_view key,
                                    const std::vector<std::string>& subprotocols,
                                    ws_options client_opts);
std::string ws_make_random_key();

// Validate a server's 101 response against `key`, and configure deflate on
// `state` per the negotiated extensions. Throws ws_error on failure.
void ws_validate_handshake_response(ws_state* state,
                                    const http_message& resp,
                                    std::string_view key,
                                    ws_options client_opts);

// Build the HTTP/1.1 101 response bytes that complete a server-side upgrade.
// `req` must already be validated by `is_ws_upgrade_request`. Returns the
// raw response (status line + headers + final CRLF). The negotiated options
// (subset of {permessage_deflate, *_no_context_takeover}) are written to
// `negotiated_out`.
std::string ws_build_server_response(const http_message& req,
                                     const std::vector<std::string>& subprotocols,
                                     ws_options server_opts,
                                     ws_options& negotiated_out);
} // namespace detail


class ws_stream {
public:
    using receive_result = std::variant<ws_message, ws_close>;

    ws_stream();
    ws_stream(ws_stream&&) noexcept;
    ws_stream& operator=(ws_stream&&) noexcept;
    ws_stream(const ws_stream&) = delete;
    ws_stream& operator=(const ws_stream&) = delete;
    ~ws_stream();

    explicit operator bool() const noexcept { return state_ != nullptr; }

    // Build a client-side ws_stream. Stream is NOT stored; it's passed
    // to handshake()/send_*/receive()/close().
    //
    // `opts` may include `permessage_deflate` (default) to advertise the
    // extension in the upgrade request. The takeover flags are ignored on
    // make_client; they are negotiated by the server's response.
    static ws_stream make_client(std::string host,
                                 std::string path,
                                 std::vector<std::string> subprotocols = {},
                                 ws_options opts = ws_options::permessage_deflate);

    // Build a server-side ws_stream after the HTTP/101 handshake has been
    // written by the caller. `residual` holds bytes already read past the
    // upgrade request. `opts` describes what the upgrade negotiation agreed
    // on: optionally `permessage_deflate` plus either or both of the
    // `*_no_context_takeover` flags.
    static ws_stream make_server(std::string residual = {},
                                 ws_options opts = ws_options::none);

    bool is_open() const noexcept;
    bool permessage_deflate_negotiated() const noexcept;

    // ---- transport-templated coroutine methods ----
    //
    // Each takes the transport by value (refcount bump for fd/tls_stream).
    // Pass the same transport every time you call these on a given stream.

    template <class Stream> task<> handshake(Stream s);

    template <class Stream> task<receive_result> receive(Stream s);

    template <class Stream> task<> send_text(Stream s, std::string_view payload);
    template <class Stream> task<> send_binary(Stream s, const void* data, size_t n);
    template <class Stream> task<> send_binary(Stream s, std::string_view payload) {
        return send_binary(std::move(s), payload.data(), payload.size());
    }
    template <class Stream> task<> send_ping(Stream s, std::string_view payload = {});
    template <class Stream> task<> send_pong(Stream s, std::string_view payload = {});

    // Queue a close frame and flush it. Does not wait for the peer's close.
    template <class Stream>
    task<> send_close(Stream s, uint16_t code = 1000, std::string_view reason = {});

    // Graceful shutdown: send_close (if not already sent), then drain
    // incoming traffic until the peer's close frame arrives or the transport
    // reaches EOF. Safe to call on an already-closed stream.
    template <class Stream> task<> close(Stream s);

private:
    // Drive the wslay event loop until it has nothing pending to write,
    // emitting bytes to the transport as it produces them.
    template <class Stream> task<> pump_writes(Stream s);

    // Pull bytes from the transport (or buffered residual) into wslay until
    // one event is dispatched. Returns false on EOF.
    template <class Stream> task<bool> pump_reads(Stream s);

    std::unique_ptr<detail::ws_state> state_;
    std::string client_host_;
    std::string client_path_;
    std::vector<std::string> client_subprotocols_;
    ws_options client_options_ = ws_options::none;
};


// Returns true iff `req` looks like a valid WebSocket upgrade request:
// HTTP/1.1, GET, with `Upgrade: websocket`, `Connection` containing
// `Upgrade`, `Sec-WebSocket-Key`, and `Sec-WebSocket-Version: 13`.
bool is_ws_upgrade_request(const http_message& req);


// Server-side handshake completion. The caller has already received `req`
// from `hp`, reading from `cfd`. This function validates the request, writes
// the HTTP/101 response on `cfd`, and returns a server-mode `ws_stream`. The
// caller continues to use `cfd` (passed by value to ws_stream methods).
//
// `opts` may include `permessage_deflate` (default) to accept the extension
// when the client offers it. Other flag bits are ignored on the request
// side; the takeover state is set on the returned ws_stream based on what
// was negotiated.
//
// On a malformed upgrade request, a 400 response is written and a ws_error
// is thrown.
template <class Stream>
task<ws_stream> ws_upgrade(http_parser& hp,
                           Stream s,
                           const http_message& req,
                           std::vector<std::string> subprotocols = {},
                           ws_options opts = ws_options::permessage_deflate);


// ===========================================================================
// Inline template definitions
// ===========================================================================

template <class Stream>
task<> ws_stream::pump_writes(Stream s) {
    if (!state_) co_return;
    while (true) {
        std::string bytes;
        bool more = detail::ws_state_pump_writes_step(state_.get(), bytes);
        if (!bytes.empty()) {
            co_await write(s, bytes.data(), bytes.size());
        }
        if (!more) {
            break;
        }
    }
}

template <class Stream>
task<bool> ws_stream::pump_reads(Stream s) {
    if (!state_) co_return false;
    if (detail::ws_state_drain_recv_buffer(state_.get())) {
        co_return true;
    }
    if (detail::ws_state_read_done(state_.get())) {
        co_return false;
    }
    constexpr size_t bufsz = 8192;
    uint8_t buf[bufsz];
    auto n = co_await read_once(s, buf, bufsz);
    if (!n || *n == 0) {
        detail::ws_state_set_recv_eof(state_.get());
        co_return false;
    }
    detail::ws_state_feed_recv(state_.get(), buf, *n);
    co_return true;
}

template <class Stream>
task<ws_stream::receive_result> ws_stream::receive(Stream s) {
    while (true) {
        co_await pump_writes(s);

        receive_result m;
        if (detail::ws_state_pop_message(state_.get(), m)) {
            co_return m;
        }

        if (detail::ws_state_read_done(state_.get())) {
            co_return detail::ws_state_make_close(state_.get());
        }

        bool ok = co_await pump_reads(s);
        if (!ok) {
            // Try to drain any final messages decoded by a closing wslay.
            if (detail::ws_state_pop_message(state_.get(), m)) {
                co_return m;
            }
            co_return detail::ws_state_make_close(state_.get());
        }
    }
}

template <class Stream>
task<> ws_stream::send_text(Stream s, std::string_view payload) {
    detail::ws_state_queue_text(state_.get(), payload);
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::send_binary(Stream s, const void* data, size_t n) {
    detail::ws_state_queue_binary(state_.get(), data, n);
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::send_ping(Stream s, std::string_view payload) {
    detail::ws_state_queue_ping(state_.get(), payload);
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::send_pong(Stream s, std::string_view payload) {
    detail::ws_state_queue_pong(state_.get(), payload);
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::send_close(Stream s, uint16_t code, std::string_view reason) {
    detail::ws_state_queue_close(state_.get(), code, reason);
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::close(Stream s) {
    if (!state_) co_return;
    if (!detail::ws_state_close_sent(state_.get())) {
        co_await send_close(s, 1000, "");
    }
    while (!detail::ws_state_read_done(state_.get())
           && !detail::ws_state_close_received(state_.get())) {
        bool ok = co_await pump_reads(s);
        if (!ok) break;
    }
    co_await pump_writes(std::move(s));
}

template <class Stream>
task<> ws_stream::handshake(Stream s) {
    if (!detail::ws_state_is_client(state_.get())) {
        throw ws_error(WSLAY_ERR_INVALID_ARGUMENT,
                       "handshake() called on non-client ws_stream");
    }

    std::string key = detail::ws_make_random_key();
    std::string req = detail::ws_build_client_request(
        client_host_, client_path_, key,
        client_subprotocols_, client_options_);

    co_await write(s, req.data(), req.size());

    // Parse the 101 response with http_parser. The transport is refcounted,
    // so passing `t` by value is a refcount bump that keeps the connection
    // alive across the parser's awaits.
    http_parser hp(http_parser::client);
    auto resp = co_await hp.receive(s);
    if (!hp.ok()) {
        throw ws_error(WSLAY_ERR_PROTO,
                       std::string("ws handshake: parse error: ")
                       + hp.error_name());
    }
    if (resp.status_code() != 101) {
        throw ws_error(WSLAY_ERR_PROTO,
                       std::format("ws handshake: server returned {} {}",
                                   resp.status_code(), resp.status_message()));
    }

    detail::ws_validate_handshake_response(state_.get(), resp, key,
                                           client_options_);

    // Stash any bytes that arrived alongside the 101 (a server might have
    // piggybacked a frame on the same TCP segment) into wslay's recv buffer
    // before we let it pull more from the transport.
    detail::ws_state_set_residual(state_.get(),
                                  std::move(hp.receive_buffer()));
}

template <class Stream>
task<ws_stream> ws_upgrade(http_parser& hp,
                           Stream s,
                           const http_message& req,
                           std::vector<std::string> subprotocols,
                           ws_options opts) {
    if (!is_ws_upgrade_request(req)) {
        http_message err;
        err.status_code(400)
            .header("Connection", "close")
            .header("Content-Type", "text/plain")
            .body("Bad WebSocket upgrade request\n");
        hp.clear_should_keep_alive();
        co_await hp.send_response(s, std::move(err));
        throw ws_error(WSLAY_ERR_PROTO, "bad ws upgrade request");
    }

    ws_options negotiated = ws_options::none;
    std::string res = detail::ws_build_server_response(
        req, subprotocols, opts, negotiated);

    co_await write(s, res.data(), res.size());

    co_return ws_stream::make_server(std::move(hp.receive_buffer()), negotiated);
}

} // namespace cotamer
