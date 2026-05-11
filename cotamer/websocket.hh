#pragma once
#include "cotamer/cotamer.hh"
#include "cotamer/config.hh"
#include "cotamer/http.hh"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// cotamer/websocket.hh
//    Coroutine-driven WebSocket (RFC 6455) streams built on wslay.
//
//    `ws_parser` wraps a connected byte-stream (a `cotamer::stream`, e.g.
//    a `cotamer::fd` for ws:// or a `cotamer::tls_stream` for wss://) and
//    provides coroutine-based send/receive on top of wslay's framing.
//
//    Server-side use:
//        cot::http_parser hp(std::move(cfd), cot::http_parser::server);
//        auto req = co_await hp.receive();
//        if (cot::is_ws_upgrade_request(req)) {
//            auto ws = co_await cot::ws_upgrade(std::move(hp), req);
//            // ... use ws ...
//        }
//
//    Client-side use:
//        auto cfd = co_await cot::tcp_connect("host:port");
//        auto ws = cot::ws_parser::wrap_client(std::move(cfd),
//                                              "host", "/path");
//        co_await ws.handshake();
//        // ... use ws ...

namespace cotamer {

// Errors

const std::error_category& ws_error_category() noexcept;

class ws_error : public std::system_error {
public:
    explicit ws_error(int wslay_errcode);
    ws_error(int wslay_errcode, std::string what);
};


enum class ws_opcode : uint8_t {
    continuation = 0x0, text = 0x1, binary = 0x2,
    close = 0x8, ping = 0x9, pong = 0xA
};

// Bit flags describing the negotiated state of an active WebSocket connection
// (RFC 7692 permessage-deflate). `permessage_deflate` is set when the
// extension was negotiated; `server_no_context_takeover` and
// `client_no_context_takeover` carry the literal RFC 7692 parameter
// semantics — whether the server (resp. client) resets its compression
// context after every message it sends.
enum class ws_connection_options : unsigned {
    none = 0,
    permessage_deflate = 1,
    server_no_context_takeover = 2,
    client_no_context_takeover = 4,
    unknown_option = 8
};
inline ws_connection_options operator&(ws_connection_options a, ws_connection_options b) {
    return static_cast<ws_connection_options>(unsigned(a) & unsigned(b));
}
inline ws_connection_options operator|(ws_connection_options a, ws_connection_options b) {
    return static_cast<ws_connection_options>(unsigned(a) | unsigned(b));
}
inline ws_connection_options& operator|=(ws_connection_options& a, ws_connection_options b) {
    a = a | b;
    return a;
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

namespace detail { struct ws_state; }


class ws_parser {
public:
    using receive_result = std::variant<ws_message, ws_close>;

    ws_parser(ws_parser&&) noexcept;
    ws_parser& operator=(ws_parser&&) noexcept;
    ws_parser(const ws_parser&) = delete;
    ws_parser& operator=(const ws_parser&) = delete;
    ~ws_parser();

    explicit operator bool() const noexcept { return state_ != nullptr; }

    // Wrap an already-connected transport in a client-side ws_parser.
    // The handshake is NOT performed; call `handshake()`.
    //
    // `offer_options` controls extension negotiation. Setting
    // `permessage_deflate` (the default) advertises the permessage-deflate
    // extension (RFC 7692) in the upgrade request. Additionally setting
    // `server_no_context_takeover` and/or `client_no_context_takeover`
    // asks the server to negotiate those parameters.
    static ws_parser wrap_client(std::unique_ptr<stream> strm,
                                 std::string host,
                                 std::string path,
                                 std::vector<std::string> subprotocols = {},
                                 ws_connection_options offer_options =
                                     ws_connection_options::permessage_deflate);
    template <typename S>
    static ws_parser wrap_client(S strm, std::string host, std::string path, std::vector<std::string> subprotocols = {}, ws_connection_options offer_options = ws_connection_options::permessage_deflate) {
        return wrap_client(make_stream(std::move(strm)), std::move(host), std::move(path), std::move(subprotocols), offer_options);
    }

    // Wrap a transport that is already past the HTTP/101 handshake (caller
    // performed/validated the handshake) in a server-side ws_parser.
    // `residual` holds any bytes the caller already read past the end of the
    // upgrade request (e.g. WebSocket frames piggybacked on the same TCP
    // segment); they are pushed into the receive pipeline before any further
    // transport reads.
    //
    // `options` records the outcome of the upgrade negotiation: whether
    // permessage-deflate was enabled and which side(s) must reset their
    // compression context after every message (per the parameters echoed in
    // the 101 response).
    static ws_parser wrap_server(std::unique_ptr<stream> strm,
                                 std::string residual = {},
                                 ws_connection_options options = {});
    template <typename S>
    static ws_parser wrap_server(S strm, std::string residual = {}, ws_connection_options options = {}) {
        return wrap_server(make_stream(std::move(strm)), std::move(residual), options);
    }

    // Client-side handshake: sends the upgrade request, parses the 101
    // response, validates Sec-WebSocket-Accept. Throws on failure.
    task<> handshake();

    // Receive one logical message. Auto-pongs to incoming pings and processes
    // close handshakes internally. When the peer closes (or the transport
    // reaches EOF), returns a `ws_close` instead of a `ws_message`.
    task<receive_result> receive();

    task<> send_text(std::string_view payload);
    task<> send_binary(const void* data, size_t n);
    inline task<> send_binary(std::string_view payload) {
        return send_binary(payload.data(), payload.size());
    }
    task<> send_ping(std::string_view payload = {});
    task<> send_pong(std::string_view payload = {});

    // Queue a close frame and flush it. Does not wait for the peer's close.
    task<> send_close(uint16_t code = 1000, std::string_view reason = {});

    // Graceful shutdown: send_close (if not already sent), then drain
    // incoming traffic until the peer's close frame arrives or the transport
    // reaches EOF. Safe to call on an already-closed stream.
    task<> close();

    bool is_open() const noexcept;

    // True iff the permessage-deflate extension was negotiated for this
    // connection.
    bool permessage_deflate_negotiated() const noexcept;

private:
    ws_parser(std::unique_ptr<stream> strm, bool is_client);

    std::unique_ptr<stream> stream_;
    std::unique_ptr<detail::ws_state> state_;
    std::string client_host_;
    std::string client_path_;
    std::vector<std::string> client_subprotocols_;
    ws_connection_options client_offer_options_ = {};

    task<> pump_writes();
    task<bool> pump_reads();   // returns false on EOF
};


// Returns true iff `req` looks like a valid WebSocket upgrade request:
// HTTP/1.1, GET, with `Upgrade: websocket`, `Connection` containing
// `Upgrade`, `Sec-WebSocket-Key`, and `Sec-WebSocket-Version: 13`.
bool is_ws_upgrade_request(const http_message& req);

// Server-side handshake completion. The caller has already received the
// upgrade request `req` from `hp`. This function validates the request,
// writes the HTTP/101 response, takes ownership of the underlying fd, and
// returns a server-mode ws_parser ready for receive()/send.
//
// `subprotocols` is the server's list of supported subprotocols; the first
// entry from `Sec-WebSocket-Protocol` that appears in this list is selected
// (or none, in which case no subprotocol header is sent).
//
// `accept_options` controls extension negotiation. Set
// `permessage_deflate` (the default) to negotiate permessage-deflate when
// offered; clear it to refuse. Setting `server_no_context_takeover` and/or
// `client_no_context_takeover` forces those parameters into the response
// even if the client didn't request them.
//
// On a malformed upgrade request, a 400 response is written and a ws_error
// is thrown.
task<ws_parser> ws_upgrade(http_parser&& hp, const http_message& req,
                           std::vector<std::string> subprotocols = {},
                           ws_connection_options accept_options =
                               ws_connection_options::permessage_deflate);

} // namespace cotamer
