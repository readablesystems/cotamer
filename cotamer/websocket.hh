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
//    `basic_ws_stream<Transport>` wraps a connected byte-stream transport
//    (typically `cotamer::fd` for ws://, or `cotamer::tls_stream` for wss://)
//    and provides coroutine-based send/receive on top of wslay's framing.
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
//        auto ws = cot::ws_stream::wrap_client(std::move(cfd),
//                                              "host", "/path");
//        co_await ws.handshake();
//        // ... use ws ...

namespace cotamer {

enum class ws_opcode : uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA
};

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

// Thrown on protocol or callback errors. Wraps a wslay error code (negative).
class ws_error : public std::runtime_error {
public:
    explicit ws_error(int wslay_err);
    explicit ws_error(int wslay_err, const std::string& what);
    int wslay_error() const noexcept { return err_; }
private:
    int err_;
};

namespace detail { struct ws_state; }


template <class Transport>
class basic_ws_stream {
public:
    using receive_result = std::variant<ws_message, ws_close>;

    basic_ws_stream(basic_ws_stream&&) noexcept;
    basic_ws_stream& operator=(basic_ws_stream&&) noexcept;
    basic_ws_stream(const basic_ws_stream&) = delete;
    basic_ws_stream& operator=(const basic_ws_stream&) = delete;
    ~basic_ws_stream();

    explicit operator bool() const noexcept { return state_ != nullptr; }

    // Wrap an already-connected transport in a client-side ws_stream.
    // The handshake is NOT performed; call `handshake()`.
    //
    // `offer_permessage_deflate` (default true) controls whether the client
    // advertises the permessage-deflate extension (RFC 7692) in the upgrade
    // request. When the server accepts, both sides transparently compress
    // data frames; otherwise the connection runs uncompressed.
    static basic_ws_stream wrap_client(Transport t,
                                       std::string host,
                                       std::string path,
                                       std::vector<std::string> subprotocols = {},
                                       bool offer_permessage_deflate = true);

    // Wrap a transport that is already past the HTTP/101 handshake (caller
    // performed/validated the handshake) in a server-side ws_stream.
    // `residual` holds any bytes the caller already read past the end of the
    // upgrade request (e.g. WebSocket frames piggybacked on the same TCP
    // segment); they are pushed into the receive pipeline before any further
    // transport reads.
    //
    // `permessage_deflate` indicates whether the upgrade negotiation enabled
    // permessage-deflate. The optional `*_no_context_takeover` flags indicate
    // whether each side resets its compression context after every message
    // (per the parameters echoed in the 101 response).
    static basic_ws_stream wrap_server(Transport t,
                                       std::string residual = {},
                                       bool permessage_deflate = false,
                                       bool inbound_no_context_takeover = false,
                                       bool outbound_no_context_takeover = false);

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

    Transport& transport() noexcept { return t_; }
    const Transport& transport() const noexcept { return t_; }

private:
    basic_ws_stream(Transport t, bool is_client);

    Transport t_;
    std::unique_ptr<detail::ws_state> state_;
    std::string client_host_;
    std::string client_path_;
    std::vector<std::string> client_subprotocols_;
    bool client_offer_deflate_ = false;

    task<> pump_writes();
    task<bool> pump_reads();   // returns false on EOF
};


using ws_stream = basic_ws_stream<fd>;

// `wss_stream` (basic_ws_stream<tls_stream>) is planned for a later phase;
// it requires linking both `CotamerTls` and `CotamerWebSocket` together.


// Returns true iff `req` looks like a valid WebSocket upgrade request:
// HTTP/1.1, GET, with `Upgrade: websocket`, `Connection` containing
// `Upgrade`, `Sec-WebSocket-Key`, and `Sec-WebSocket-Version: 13`.
bool is_ws_upgrade_request(const http_message& req);

// Server-side handshake completion. The caller has already received the
// upgrade request `req` from `hp`. This function validates the request,
// writes the HTTP/101 response, takes ownership of the underlying fd, and
// returns a server-mode ws_stream ready for receive()/send.
//
// `subprotocols` is the server's list of supported subprotocols; the first
// entry from `Sec-WebSocket-Protocol` that appears in this list is selected
// (or none, in which case no subprotocol header is sent).
//
// `accept_permessage_deflate` (default true) controls whether the server
// negotiates the permessage-deflate extension when offered by the client.
//
// On a malformed upgrade request, a 400 response is written and a ws_error
// is thrown.
task<ws_stream> ws_upgrade(http_parser&& hp, const http_message& req,
                           std::vector<std::string> subprotocols = {},
                           bool accept_permessage_deflate = true);

} // namespace cotamer
