// -*- mode: c++ -*-
#include "cotamer/websocket.hh"
#include "cotamer/io.hh"

#include <wslay/wslay.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <random>
#include <string>
#include <vector>

namespace cotamer {

// --- error -----------------------------------------------------------------

namespace {
const char* wslay_error_message(int err) {
    switch (err) {
    case WSLAY_ERR_WANT_READ:        return "want read";
    case WSLAY_ERR_WANT_WRITE:       return "want write";
    case WSLAY_ERR_PROTO:            return "protocol error";
    case WSLAY_ERR_INVALID_ARGUMENT: return "invalid argument";
    case WSLAY_ERR_INVALID_CALLBACK: return "invalid callback";
    case WSLAY_ERR_NO_MORE_MSG:      return "no more messages allowed";
    case WSLAY_ERR_CALLBACK_FAILURE: return "callback failure";
    case WSLAY_ERR_WOULDBLOCK:       return "would block";
    case WSLAY_ERR_NOMEM:            return "out of memory";
    default:                         return "wslay error";
    }
}
} // namespace

ws_error::ws_error(int err)
    : std::runtime_error(wslay_error_message(err)), err_(err) {
}
ws_error::ws_error(int err, const std::string& what)
    : std::runtime_error(what), err_(err) {
}


// --- SHA-1 (RFC 3174 reference, public domain) -----------------------------
// Steve Reid's well-known implementation, condensed.

namespace {

struct sha1_state {
    uint32_t h[5];
    uint64_t bits;
    uint8_t buf[64];
    size_t buflen;
};

inline uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

void sha1_init(sha1_state& s) {
    s.h[0] = 0x67452301;
    s.h[1] = 0xEFCDAB89;
    s.h[2] = 0x98BADCFE;
    s.h[3] = 0x10325476;
    s.h[4] = 0xC3D2E1F0;
    s.bits = 0;
    s.buflen = 0;
}

void sha1_compress(sha1_state& s, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i*4]) << 24)
             | (uint32_t(block[i*4+1]) << 16)
             | (uint32_t(block[i*4+2]) << 8)
             | uint32_t(block[i*4+3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e;
}

void sha1_update(sha1_state& s, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s.bits += uint64_t(len) * 8;
    while (len > 0) {
        size_t n = std::min(len, size_t(64 - s.buflen));
        std::memcpy(s.buf + s.buflen, p, n);
        s.buflen += n; p += n; len -= n;
        if (s.buflen == 64) {
            sha1_compress(s, s.buf);
            s.buflen = 0;
        }
    }
}

void sha1_final(sha1_state& s, uint8_t out[20]) {
    uint64_t bits = s.bits;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    while (s.buflen != 56) {
        uint8_t z = 0;
        sha1_update(s, &z, 1);
    }
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) {
        lenbuf[i] = (bits >> (56 - i*8)) & 0xff;
    }
    sha1_update(s, lenbuf, 8);
    for (int i = 0; i < 5; ++i) {
        out[i*4]   = (s.h[i] >> 24) & 0xff;
        out[i*4+1] = (s.h[i] >> 16) & 0xff;
        out[i*4+2] = (s.h[i] >>  8) & 0xff;
        out[i*4+3] =  s.h[i]        & 0xff;
    }
}


// --- Base64 ---------------------------------------------------------------

constexpr char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i+1]) << 8) | p[i+2];
        out.push_back(b64_chars[(v >> 18) & 0x3f]);
        out.push_back(b64_chars[(v >> 12) & 0x3f]);
        out.push_back(b64_chars[(v >>  6) & 0x3f]);
        out.push_back(b64_chars[ v        & 0x3f]);
    }
    if (i < n) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= uint32_t(p[i+1]) << 8;
        out.push_back(b64_chars[(v >> 18) & 0x3f]);
        out.push_back(b64_chars[(v >> 12) & 0x3f]);
        if (i + 1 < n) {
            out.push_back(b64_chars[(v >> 6) & 0x3f]);
            out.push_back('=');
        } else {
            out += "==";
        }
    }
    return out;
}


// --- Sec-WebSocket-Accept --------------------------------------------------

constexpr const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string ws_compute_accept(std::string_view key) {
    sha1_state s;
    sha1_init(s);
    sha1_update(s, key.data(), key.size());
    sha1_update(s, ws_magic, sizeof(ws_magic) - 1);
    uint8_t digest[20];
    sha1_final(s, digest);
    return base64_encode(digest, 20);
}


// --- header utilities ------------------------------------------------------

bool header_contains_ci_token(std::string_view value, std::string_view token) {
    auto match = [&](size_t pos) {
        if (pos + token.size() > value.size()) return false;
        for (size_t i = 0; i < token.size(); ++i) {
            char a = value[pos + i], b = token[i];
            if (a >= 'A' && a <= 'Z') a = a + 32;
            if (b >= 'A' && b <= 'Z') b = b + 32;
            if (a != b) return false;
        }
        char before = pos == 0 ? ',' : value[pos - 1];
        char after = pos + token.size() == value.size()
                     ? ',' : value[pos + token.size()];
        auto sep = [](char c) { return c == ',' || c == ' ' || c == '\t'; };
        return sep(before) && sep(after);
    };
    for (size_t i = 0; i + token.size() <= value.size(); ++i) {
        if (match(i)) return true;
    }
    return false;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

} // namespace


// --- ws_state --------------------------------------------------------------

namespace detail {

struct ws_state {
    wslay_event_context_ptr ctx = nullptr;
    bool is_client = false;

    // Inbound bytes pulled from the transport, awaiting wslay_event_recv()
    // consumption. recv_pos is the next byte to deliver.
    std::vector<uint8_t> recv_buf;
    size_t recv_pos = 0;
    bool recv_eof = false;

    // Outbound bytes wslay has serialized but we haven't yet flushed to the
    // transport.
    std::vector<uint8_t> send_buf;

    // Completed messages (data + close), enqueued by on_msg_recv_callback
    // and consumed by receive().
    std::deque<std::variant<ws_message, ws_close>> msg_queue;

    // Used to assemble fragmented data messages when no_buffering is off.
    // wslay actually buffers for us, but we keep this here in case we need
    // streaming later.

    // Stream the user is currently waiting on; used by send_text() etc. to
    // know when to stop pumping.
    ~ws_state() {
        if (ctx) {
            wslay_event_context_free(ctx);
        }
    }
};

} // namespace detail


// --- wslay <-> Cotamer glue (synchronous callbacks) -----------------------

namespace {

ssize_t cb_recv(wslay_event_context_ptr ctx, uint8_t* buf, size_t len,
                int /*flags*/, void* user_data) {
    auto* s = static_cast<detail::ws_state*>(user_data);
    size_t avail = s->recv_buf.size() - s->recv_pos;
    if (avail == 0) {
        if (s->recv_eof) {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    size_t n = std::min(len, avail);
    std::memcpy(buf, s->recv_buf.data() + s->recv_pos, n);
    s->recv_pos += n;
    if (s->recv_pos == s->recv_buf.size()) {
        s->recv_buf.clear();
        s->recv_pos = 0;
    }
    return ssize_t(n);
}

ssize_t cb_send(wslay_event_context_ptr /*ctx*/, const uint8_t* data,
                size_t len, int /*flags*/, void* user_data) {
    auto* s = static_cast<detail::ws_state*>(user_data);
    s->send_buf.insert(s->send_buf.end(), data, data + len);
    return ssize_t(len);
}

int cb_genmask(wslay_event_context_ptr /*ctx*/, uint8_t* buf, size_t len,
               void* /*user_data*/) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (size_t i = 0; i < len; ++i) {
        buf[i] = uint8_t(rng() & 0xff);
    }
    return 0;
}

void cb_on_msg_recv(wslay_event_context_ptr /*ctx*/,
                    const wslay_event_on_msg_recv_arg* arg, void* user_data) {
    auto* s = static_cast<detail::ws_state*>(user_data);
    if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        ws_close c;
        c.code = arg->status_code;
        if (arg->msg_length > 2) {
            // Per RFC 6455, payload is: 2-byte status code + UTF-8 reason.
            c.reason.assign(reinterpret_cast<const char*>(arg->msg) + 2,
                            arg->msg_length - 2);
        }
        s->msg_queue.emplace_back(std::move(c));
    } else if (arg->opcode == WSLAY_TEXT_FRAME
               || arg->opcode == WSLAY_BINARY_FRAME) {
        ws_message m;
        m.opcode = arg->opcode == WSLAY_TEXT_FRAME ? ws_opcode::text
                                                   : ws_opcode::binary;
        m.payload.assign(reinterpret_cast<const char*>(arg->msg),
                         arg->msg_length);
        s->msg_queue.emplace_back(std::move(m));
    }
    // ping/pong are auto-handled by wslay; we simply ignore them here
}

const wslay_event_callbacks ws_event_callbacks = {
    cb_recv,
    cb_send,
    cb_genmask,
    nullptr,                // on_frame_recv_start_callback
    nullptr,                // on_frame_recv_chunk_callback
    nullptr,                // on_frame_recv_end_callback
    cb_on_msg_recv
};

} // namespace


// --- transport adapters ----------------------------------------------------

namespace detail {

// Read up to n bytes from a transport. Returns 0 at EOF.
inline task<size_t> ws_xport_read(fd& f, void* buf, size_t n) {
    co_return co_await read_once(f, buf, n);
}
inline task<size_t> ws_xport_write(fd& f, const void* buf, size_t n) {
    co_return co_await write(f, buf, n);
}

} // namespace detail


// --- basic_ws_stream -------------------------------------------------------

template <class Transport>
basic_ws_stream<Transport>::basic_ws_stream(basic_ws_stream&&) noexcept = default;
template <class Transport>
basic_ws_stream<Transport>& basic_ws_stream<Transport>::operator=(basic_ws_stream&&) noexcept = default;
template <class Transport>
basic_ws_stream<Transport>::~basic_ws_stream() = default;

template <class Transport>
basic_ws_stream<Transport>::basic_ws_stream(Transport t, bool is_client)
    : t_(std::move(t)),
      state_(std::make_unique<detail::ws_state>()) {
    state_->is_client = is_client;
    int rv = is_client
        ? wslay_event_context_client_init(&state_->ctx,
                                          &ws_event_callbacks,
                                          state_.get())
        : wslay_event_context_server_init(&state_->ctx,
                                          &ws_event_callbacks,
                                          state_.get());
    if (rv != 0) throw ws_error(rv);
}

template <class Transport>
basic_ws_stream<Transport> basic_ws_stream<Transport>::wrap_client(
    Transport t, std::string host, std::string path,
    std::vector<std::string> subprotocols)
{
    basic_ws_stream s(std::move(t), /*is_client=*/true);
    s.client_host_ = std::move(host);
    s.client_path_ = std::move(path);
    s.client_subprotocols_ = std::move(subprotocols);
    return s;
}

template <class Transport>
basic_ws_stream<Transport> basic_ws_stream<Transport>::wrap_server(
    Transport t, std::string residual)
{
    basic_ws_stream s(std::move(t), /*is_client=*/false);
    if (!residual.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(residual.data());
        s.state_->recv_buf.assign(p, p + residual.size());
    }
    return s;
}

template <class Transport>
bool basic_ws_stream<Transport>::is_open() const noexcept {
    if (!state_) return false;
    return wslay_event_get_read_enabled(state_->ctx)
        || wslay_event_get_write_enabled(state_->ctx);
}

template <class Transport>
task<> basic_ws_stream<Transport>::pump_writes() {
    if (!state_) co_return;
    while (wslay_event_want_write(state_->ctx)) {
        int rv = wslay_event_send(state_->ctx);
        if (rv != 0) {
            throw ws_error(rv);
        }
        if (state_->send_buf.empty()) {
            break;
        }
        std::vector<uint8_t> tmp;
        tmp.swap(state_->send_buf);
        co_await detail::ws_xport_write(t_, tmp.data(), tmp.size());
    }
    if (!state_->send_buf.empty()) {
        std::vector<uint8_t> tmp;
        tmp.swap(state_->send_buf);
        co_await detail::ws_xport_write(t_, tmp.data(), tmp.size());
    }
}

template <class Transport>
task<bool> basic_ws_stream<Transport>::pump_reads() {
    if (!state_ || !wslay_event_get_read_enabled(state_->ctx)) {
        co_return false;
    }
    // If we already have buffered bytes (e.g. left over from the handshake
    // parse, or held over from a previous wslay_event_recv that produced a
    // message and then would have woudblocked), drain them through wslay
    // before doing another transport read.
    if (state_->recv_buf.size() > state_->recv_pos) {
        int rv = wslay_event_recv(state_->ctx);
        if (rv != 0) {
            throw ws_error(rv);
        }
        co_return true;
    }
    constexpr size_t bufsz = 8192;
    uint8_t buf[bufsz];
    size_t n = co_await detail::ws_xport_read(t_, buf, bufsz);
    if (n == 0) {
        state_->recv_eof = true;
        wslay_event_shutdown_read(state_->ctx);
        co_return false;
    }
    state_->recv_buf.insert(state_->recv_buf.end(), buf, buf + n);
    int rv = wslay_event_recv(state_->ctx);
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_return true;
}

template <class Transport>
task<typename basic_ws_stream<Transport>::receive_result>
basic_ws_stream<Transport>::receive() {
    while (true) {
        co_await pump_writes();

        if (!state_->msg_queue.empty()) {
            auto m = std::move(state_->msg_queue.front());
            state_->msg_queue.pop_front();
            co_return m;
        }

        if (!wslay_event_get_read_enabled(state_->ctx)
            || state_->recv_eof) {
            // Read side is done, no more messages will arrive. If we got a
            // close frame it should already be on the queue; otherwise this
            // is an abnormal close.
            ws_close c;
            c.code = wslay_event_get_close_received(state_->ctx)
                ? wslay_event_get_status_code_received(state_->ctx)
                : WSLAY_CODE_ABNORMAL_CLOSURE;
            co_return c;
        }

        bool ok = co_await pump_reads();
        if (!ok && state_->msg_queue.empty()) {
            ws_close c;
            c.code = wslay_event_get_close_received(state_->ctx)
                ? wslay_event_get_status_code_received(state_->ctx)
                : WSLAY_CODE_ABNORMAL_CLOSURE;
            co_return c;
        }
    }
}

template <class Transport>
task<> basic_ws_stream<Transport>::send_text(std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_TEXT_FRAME,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) throw ws_error(rv);
    co_await pump_writes();
}

template <class Transport>
task<> basic_ws_stream<Transport>::send_binary(const void* data, size_t n) {
    wslay_event_msg msg = {
        WSLAY_BINARY_FRAME,
        static_cast<const uint8_t*>(data),
        n
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) throw ws_error(rv);
    co_await pump_writes();
}

template <class Transport>
task<> basic_ws_stream<Transport>::send_ping(std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PING,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) throw ws_error(rv);
    co_await pump_writes();
}

template <class Transport>
task<> basic_ws_stream<Transport>::send_pong(std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PONG,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) throw ws_error(rv);
    co_await pump_writes();
}

template <class Transport>
task<> basic_ws_stream<Transport>::send_close(uint16_t code,
                                              std::string_view reason) {
    int rv = wslay_event_queue_close(
        state_->ctx, code,
        reinterpret_cast<const uint8_t*>(reason.data()),
        reason.size());
    if (rv != 0 && rv != WSLAY_ERR_NO_MORE_MSG) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

template <class Transport>
task<> basic_ws_stream<Transport>::close() {
    if (!state_) co_return;
    if (!wslay_event_get_close_sent(state_->ctx)) {
        co_await send_close(1000, "");
    }
    // Drain incoming traffic until peer sends close or transport EOFs.
    while (wslay_event_get_read_enabled(state_->ctx)
           && !wslay_event_get_close_received(state_->ctx)) {
        bool ok = co_await pump_reads();
        if (!ok) break;
    }
    co_await pump_writes();
}


// --- client handshake ------------------------------------------------------

namespace {

std::string make_random_key() {
    uint8_t key[16];
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (size_t i = 0; i < 16; ++i) {
        key[i] = uint8_t(rng() & 0xff);
    }
    return base64_encode(key, 16);
}

} // namespace

template <class Transport>
task<> basic_ws_stream<Transport>::handshake() {
    if (!state_ || !state_->is_client) {
        throw ws_error(WSLAY_ERR_INVALID_ARGUMENT,
                       "handshake() called on non-client ws_stream");
    }

    std::string key = make_random_key();

    std::string req = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        client_path_, client_host_, key);
    if (!client_subprotocols_.empty()) {
        req += "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < client_subprotocols_.size(); ++i) {
            if (i) req += ", ";
            req += client_subprotocols_[i];
        }
        req += "\r\n";
    }
    req += "\r\n";

    co_await detail::ws_xport_write(t_, req.data(), req.size());

    // Parse the 101 response with http_parser. Today we only instantiate
    // basic_ws_stream<fd>, so http_parser (which is fd-bound) suffices —
    // wss:// (basic_ws_stream<tls_stream>) is a Phase 4 task and will
    // require either specializing this method for fd or making http_parser
    // transport-agnostic.
    http_parser hp(t_, HTTP_RESPONSE);
    auto resp = co_await hp.receive();
    if (!hp.ok()) {
        throw ws_error(WSLAY_ERR_PROTO,
                       std::string("ws handshake: parse error: ")
                       + llhttp_errno_name(hp.error()));
    }
    if (resp.status_code() != 101) {
        throw ws_error(WSLAY_ERR_PROTO,
                       std::format("ws handshake: server returned {} {}",
                                   resp.status_code(), resp.status_message()));
    }

    bool saw_upgrade = false, saw_connection = false;
    std::string accept;
    for (auto it = resp.header_begin(); it != resp.header_end(); ++it) {
        if (it.name_eq_case("Upgrade")) {
            saw_upgrade = ieq(it.value(), "websocket");
        } else if (it.name_eq_case("Connection")) {
            saw_connection = header_contains_ci_token(it.value(), "upgrade");
        } else if (it.name_eq_case("Sec-WebSocket-Accept")) {
            accept.assign(it.value().data(), it.value().size());
        }
    }
    if (!saw_upgrade || !saw_connection) {
        throw ws_error(WSLAY_ERR_PROTO,
                       "ws handshake: missing Upgrade or Connection header");
    }
    auto expected = ws_compute_accept(key);
    if (accept != expected) {
        throw ws_error(WSLAY_ERR_PROTO,
                       "ws handshake: bad Sec-WebSocket-Accept");
    }

    // Stash any bytes that arrived alongside the 101 (a server might have
    // piggybacked a frame on the same TCP segment) into wslay's recv buffer
    // before we let it pull more from the transport.
    std::string residual = std::move(hp).take_receive_buffer();
    if (!residual.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(residual.data());
        state_->recv_buf.insert(state_->recv_buf.end(),
                                p, p + residual.size());
    }
}


// --- explicit instantiations -----------------------------------------------

template class basic_ws_stream<fd>;


// --- server upgrade --------------------------------------------------------

bool is_ws_upgrade_request(const http_message& req) {
    if (req.method() != HTTP_GET) return false;
    if (req.http_major() < 1 || (req.http_major() == 1 && req.http_minor() < 1)) {
        return false;
    }
    auto upg = req.find_header("upgrade");
    if (upg == req.header_end() || !ieq(upg.value(), "websocket")) return false;
    auto conn = req.find_header("connection");
    if (conn == req.header_end()
        || !header_contains_ci_token(conn.value(), "upgrade")) return false;
    auto key = req.find_header("sec-websocket-key");
    if (key == req.header_end() || key.value().empty()) return false;
    auto ver = req.find_header("sec-websocket-version");
    if (ver == req.header_end() || ver.value() != "13") return false;
    return true;
}

namespace {

task<void> write_400(http_parser& hp, const std::string& reason) {
    http_message res;
    res.status_code(400)
        .header("Connection", "close")
        .header("Content-Type", "text/plain")
        .body(reason + "\n");
    hp.clear_should_keep_alive();
    co_await hp.send_response(std::move(res));
}

} // namespace

task<ws_stream> ws_upgrade(http_parser&& hp, const http_message& req,
                           std::vector<std::string> subprotocols) {
    if (!is_ws_upgrade_request(req)) {
        co_await write_400(hp, "Bad WebSocket upgrade request");
        throw ws_error(WSLAY_ERR_PROTO, "bad ws upgrade request");
    }

    std::string key{req.find_header("sec-websocket-key").value()};
    std::string accept = ws_compute_accept(key);

    // Pick a subprotocol if both sides support one.
    std::string chosen_subprotocol;
    if (!subprotocols.empty()) {
        auto offered = req.find_header("sec-websocket-protocol");
        if (offered != req.header_end()) {
            std::string_view all = offered.value();
            size_t i = 0;
            while (i < all.size() && chosen_subprotocol.empty()) {
                while (i < all.size() && (all[i] == ' ' || all[i] == ',')) ++i;
                size_t j = i;
                while (j < all.size() && all[j] != ',') ++j;
                size_t end = j;
                while (end > i && (all[end-1] == ' ' || all[end-1] == '\t')) --end;
                std::string_view tok(all.data() + i, end - i);
                for (auto& sup : subprotocols) {
                    if (tok == sup) {
                        chosen_subprotocol = sup;
                        break;
                    }
                }
                i = j + 1;
            }
        }
    }

    // Build and write the 101 response by hand. We can't use http_parser's
    // send_response (which adds Content-Length etc. and assumes keep-alive
    // semantics that don't apply to an upgrade).
    std::string res = std::format(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {}\r\n",
        accept);
    if (!chosen_subprotocol.empty()) {
        res += "Sec-WebSocket-Protocol: ";
        res += chosen_subprotocol;
        res += "\r\n";
    }
    res += "\r\n";

    // Recover any bytes that arrived past the end of the upgrade headers
    // (e.g. a frame piggybacked on the same TCP segment). They belong to
    // the WebSocket protocol now.
    std::string residual = std::move(hp).take_receive_buffer();
    fd cfd = std::move(hp).release_fd();
    co_await write(cfd, res.data(), res.size());

    co_return ws_stream::wrap_server(std::move(cfd), std::move(residual));
}

} // namespace cotamer
