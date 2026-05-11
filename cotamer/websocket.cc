// -*- mode: c++ -*-
#include "cotamer/websocket.hh"
#include "cotamer/http_fields.hh"
#include "cotamer/io.hh"

#include <wslay/wslay.h>
#if COTAMER_HAVE_ZLIB
# include <zlib.h>
#endif

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

class ws_error_category_impl : public std::error_category {
public:
    const char* name() const noexcept override {
        return "wslay";
    }
    std::string message(int value) const override {
        return wslay_error_message(value);
    }
};
} // namespace

const std::error_category& ws_error_category() noexcept {
    static ws_error_category_impl cat;
    return cat;
}

ws_error::ws_error(int wslay_errcode)
    : std::system_error(std::error_code(wslay_errcode, ws_error_category())) {
}

ws_error::ws_error(int wslay_errcode, std::string what)
    : std::system_error(std::error_code(wslay_errcode, ws_error_category()), std::move(what)) {
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
        if (i + 1 < n) {
            v |= uint32_t(p[i+1]) << 8;
        }
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


} // namespace


// --- ws_state --------------------------------------------------------------

namespace detail {

#if COTAMER_HAVE_ZLIB
// Per-direction permessage-deflate streams. Use raw deflate (negative
// windowBits) per RFC 7692 § 7.2.1: the wire format omits the zlib header.
class deflate_compressor {
public:
    deflate_compressor() {
        std::memset(&strm_, 0, sizeof(strm_));
        if (deflateInit2(&strm_, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            throw ws_error(WSLAY_ERR_NOMEM, "deflateInit2 failed");
        }
    }
    deflate_compressor(const deflate_compressor&) = delete;
    deflate_compressor& operator=(const deflate_compressor&) = delete;
    ~deflate_compressor() { deflateEnd(&strm_); }

    // Compress `data[0..n)` to a buffer ending with the trailing
    // `00 00 ff ff` stripped (RFC 7692 § 7.2.1 step 4). When
    // `no_context_takeover` is true, reset the deflate dictionary first
    // so the next message uses no historical context.
    std::vector<uint8_t> compress(const void* data, size_t n,
                                  bool no_context_takeover) {
        if (no_context_takeover) {
            deflateReset(&strm_);
        }
        strm_.next_in = static_cast<Bytef*>(const_cast<void*>(data));
        strm_.avail_in = uInt(n);

        std::vector<uint8_t> out;
        out.resize(std::max<size_t>(64, n + 16));
        strm_.next_out = out.data();
        strm_.avail_out = uInt(out.size());

        while (true) {
            int rv = ::deflate(&strm_, Z_SYNC_FLUSH);
            if (rv != Z_OK && rv != Z_BUF_ERROR) {
                throw ws_error(WSLAY_ERR_PROTO, "deflate failed");
            }
            if (strm_.avail_in == 0 && strm_.avail_out > 0) {
                break;
            }
            // Need more output space.
            size_t produced = out.size() - strm_.avail_out;
            out.resize(out.size() * 2);
            strm_.next_out = out.data() + produced;
            strm_.avail_out = uInt(out.size() - produced);
        }
        size_t produced = out.size() - strm_.avail_out;
        // Strip the trailing 0x00 0x00 0xff 0xff that Z_SYNC_FLUSH emits.
        if (produced >= 4
            && out[produced - 4] == 0x00 && out[produced - 3] == 0x00
            && out[produced - 2] == 0xff && out[produced - 1] == 0xff) {
            produced -= 4;
        }
        out.resize(produced);
        return out;
    }
private:
    z_stream strm_;
};

class inflate_decompressor {
public:
    inflate_decompressor() {
        std::memset(&strm_, 0, sizeof(strm_));
        if (inflateInit2(&strm_, -15) != Z_OK) {
            throw ws_error(WSLAY_ERR_NOMEM, "inflateInit2 failed");
        }
    }
    inflate_decompressor(const inflate_decompressor&) = delete;
    inflate_decompressor& operator=(const inflate_decompressor&) = delete;
    ~inflate_decompressor() { inflateEnd(&strm_); }

    // Append `00 00 ff ff` to the wire payload (RFC 7692 § 7.2.2 step 1)
    // and inflate. When `no_context_takeover` is true, reset the inflate
    // dictionary first.
    std::vector<uint8_t> decompress(const void* data, size_t n,
                                    bool no_context_takeover) {
        if (no_context_takeover) {
            inflateReset(&strm_);
        }
        std::vector<uint8_t> input(static_cast<const uint8_t*>(data),
                                   static_cast<const uint8_t*>(data) + n);
        static const uint8_t tail[4] = {0x00, 0x00, 0xff, 0xff};
        input.insert(input.end(), tail, tail + 4);

        strm_.next_in = input.data();
        strm_.avail_in = uInt(input.size());

        std::vector<uint8_t> out;
        out.resize(std::max<size_t>(64, n * 2 + 16));
        strm_.next_out = out.data();
        strm_.avail_out = uInt(out.size());

        while (true) {
            int rv = ::inflate(&strm_, Z_SYNC_FLUSH);
            if (rv == Z_STREAM_END) {
                break;
            }
            if (rv != Z_OK && rv != Z_BUF_ERROR) {
                throw ws_error(WSLAY_ERR_PROTO, "inflate failed");
            }
            if (strm_.avail_in == 0 && strm_.avail_out > 0) {
                break;
            }
            size_t produced = out.size() - strm_.avail_out;
            out.resize(out.size() * 2);
            strm_.next_out = out.data() + produced;
            strm_.avail_out = uInt(out.size() - produced);
        }
        size_t produced = out.size() - strm_.avail_out;
        out.resize(produced);
        return out;
    }
private:
    z_stream strm_;
};
#endif // COTAMER_HAVE_ZLIB


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

    // permessage-deflate state. Compressors are allocated lazily when
    // negotiation succeeds; the `permessage_deflate` bit in `options`
    // gates their use.
    ws_connection_options options = {};
#if COTAMER_HAVE_ZLIB
    std::unique_ptr<deflate_compressor> deflate_strm;
    std::unique_ptr<inflate_decompressor> inflate_strm;
#endif

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

void cb_on_msg_recv(wslay_event_context_ptr ctx,
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
#if COTAMER_HAVE_ZLIB
        if ((arg->rsv & WSLAY_RSV1_BIT)
            && unsigned(s->options & ws_connection_options::permessage_deflate)
            && s->inflate_strm) {
            // Decompressing peer-sent data: pick the peer's bit.
            const ws_connection_options peer_bit = s->is_client
                ? ws_connection_options::server_no_context_takeover
                : ws_connection_options::client_no_context_takeover;
            try {
                auto out = s->inflate_strm->decompress(
                    arg->msg, arg->msg_length,
                    unsigned(s->options & peer_bit) != 0);
                m.payload.assign(reinterpret_cast<const char*>(out.data()),
                                 out.size());
            } catch (...) {
                // Decompression failed — fail the connection per RFC 7692.
                wslay_event_queue_close(
                    ctx, WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA,
                    nullptr, 0);
                return;
            }
        } else
#endif
        {
            m.payload.assign(reinterpret_cast<const char*>(arg->msg),
                             arg->msg_length);
        }
        s->msg_queue.emplace_back(std::move(m));
    }
    // ping/pong are auto-handled by wslay; we simply ignore them here
    (void) ctx;
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


// --- ws_parser -------------------------------------------------------------

ws_parser::ws_parser(std::unique_ptr<stream> strm, bool is_client)
    : stream_(std::move(strm)),
      state_(std::make_unique<detail::ws_state>()) {
    state_->is_client = is_client;
    int rv = is_client
        ? wslay_event_context_client_init(&state_->ctx,
                                          &ws_event_callbacks,
                                          state_.get())
        : wslay_event_context_server_init(&state_->ctx,
                                          &ws_event_callbacks,
                                          state_.get());
    if (rv != 0) {
        throw ws_error(rv);
    }
}

ws_parser::ws_parser(ws_parser&&) noexcept = default;
ws_parser& ws_parser::operator=(ws_parser&&) noexcept = default;
ws_parser::~ws_parser() = default;


ws_parser ws_parser::wrap_client(
    std::unique_ptr<stream> strm, std::string host, std::string path,
    std::vector<std::string> subprotocols,
    ws_connection_options offer_options)
{
    ws_parser ws(std::move(strm), /*is_client=*/true);
    ws.client_host_ = std::move(host);
    ws.client_path_ = std::move(path);
    ws.client_subprotocols_ = std::move(subprotocols);
    ws.client_offer_options_ = offer_options;
    return ws;
}

ws_parser ws_parser::wrap_server(
    std::unique_ptr<stream> strm, std::string residual,
    ws_connection_options options)
{
    ws_parser ws(std::move(strm), /*is_client=*/false);
    if (!residual.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(residual.data());
        ws.state_->recv_buf.assign(p, p + residual.size());
    }
#if COTAMER_HAVE_ZLIB
    if (unsigned(options & ws_connection_options::permessage_deflate)) {
        ws.state_->options = options;
        ws.state_->deflate_strm = std::make_unique<detail::deflate_compressor>();
        ws.state_->inflate_strm = std::make_unique<detail::inflate_decompressor>();
        wslay_event_config_set_allowed_rsv_bits(ws.state_->ctx, WSLAY_RSV1_BIT);
    }
#else
    (void) options;
#endif
    return ws;
}

bool ws_parser::is_open() const noexcept {
    if (!state_) {
        return false;
    }
    return wslay_event_get_read_enabled(state_->ctx)
        || wslay_event_get_write_enabled(state_->ctx);
}

bool ws_parser::permessage_deflate_negotiated() const noexcept {
    return state_
        && unsigned(state_->options & ws_connection_options::permessage_deflate);
}

task<> ws_parser::pump_writes() {
    if (!state_) {
        co_return;
    }
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
        iovec iov{ tmp.data(), tmp.size() };
        co_await stream_->sendv(&iov, 1, MSG_WAITALL);
    }
    if (!state_->send_buf.empty()) {
        std::vector<uint8_t> tmp;
        tmp.swap(state_->send_buf);
        iovec iov{ tmp.data(), tmp.size() };
        co_await stream_->sendv(&iov, 1, MSG_WAITALL);
    }
}

task<bool> ws_parser::pump_reads() {
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
    auto n = co_await stream_->recv(buf, bufsz, 0);
    if (!n || *n == 0) {
        state_->recv_eof = true;
        wslay_event_shutdown_read(state_->ctx);
        co_return false;
    }
    state_->recv_buf.insert(state_->recv_buf.end(), buf, buf + *n);
    int rv = wslay_event_recv(state_->ctx);
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_return true;
}

task<ws_parser::receive_result> ws_parser::receive() {
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

namespace {

// Switch a ws_state into permessage-deflate mode. Allocates the zlib
// streams and tells wslay to allow the RSV1 bit on incoming frames.
void enable_permessage_deflate(detail::ws_state* s,
                               ws_connection_options options) {
#if COTAMER_HAVE_ZLIB
    s->options = options | ws_connection_options::permessage_deflate;
    s->deflate_strm = std::make_unique<detail::deflate_compressor>();
    s->inflate_strm = std::make_unique<detail::inflate_decompressor>();
    wslay_event_config_set_allowed_rsv_bits(s->ctx, WSLAY_RSV1_BIT);
#else
    (void) s;
    (void) options;
    throw ws_error(WSLAY_ERR_INVALID_ARGUMENT,
                   "permessage-deflate requested but zlib is not available");
#endif
}

// Parse a Sec-WebSocket-Extensions value, looking for permessage-deflate.
// On finding it, ORs `permessage_deflate` into `opts`, plus
// `server_no_context_takeover` / `client_no_context_takeover` for any
// matching standalone parameters. If a parameter value is one we cannot
// honor, ORs in `unknown_option`.
void parse_extension_header(std::string_view header,
                            ws_connection_options& opts) {
    // The header may list several offers separated by commas. We only care
    // about the first permessage-deflate occurrence.
    for (auto ext : strings::http_value_list(header)) {
        strings::http_parameter_list params(ext);
        auto it = params.begin(), end = params.end();
        if (it == end || it.has_value()) {
            // Empty entry, or entry that begins with `name=value` rather
            // than a bare extension token — neither is a permessage-deflate
            // offering.
            continue;
        }
        if (!it.name_ci("permessage-deflate")) {
            continue;
        }

        opts |= ws_connection_options::permessage_deflate;
        for (++it; it != end; ++it) {
            if (!it.has_value()) {
                // Standalone parameter (no `=`).
                if (it.name_ci("server_no_context_takeover")) {
                    opts |= ws_connection_options::server_no_context_takeover;
                } else if (it.name_ci("client_no_context_takeover")) {
                    opts |= ws_connection_options::client_no_context_takeover;
                } else if (it.name_ci("server_max_window_bits")
                           || it.name_ci("client_max_window_bits")) {
                    // Standalone form is informational; we use 15-bit
                    // windows.
                } else {
                    opts |= ws_connection_options::unknown_option;
                }
            } else if (it.name_ci("server_max_window_bits")
                       || it.name_ci("client_max_window_bits")) {
                // We always send 15-bit windows. Anything else is an
                // unsatisfiable constraint.
                if (it.raw_value() != "15") {
                    opts |= ws_connection_options::unknown_option;
                }
            } else {
                opts |= ws_connection_options::unknown_option;
            }
        }
        return;
    }
}

// Queue a data message, applying permessage-deflate compression if enabled.
// Returns the wslay return code from the queue call.
int queue_data_message(detail::ws_state* s, uint8_t opcode,
                       const void* data, size_t n) {
#if COTAMER_HAVE_ZLIB
    if (unsigned(s->options & ws_connection_options::permessage_deflate)
        && s->deflate_strm) {
        // Compressing our outbound data: pick our own bit.
        const ws_connection_options self_bit = s->is_client
            ? ws_connection_options::client_no_context_takeover
            : ws_connection_options::server_no_context_takeover;
        auto compressed = s->deflate_strm->compress(
            data, n, unsigned(s->options & self_bit) != 0);
        wslay_event_msg msg = {
            opcode, compressed.data(), compressed.size()
        };
        return wslay_event_queue_msg_ex(s->ctx, &msg, WSLAY_RSV1_BIT);
    }
#endif
    wslay_event_msg msg = {
        opcode, static_cast<const uint8_t*>(data), n
    };
    return wslay_event_queue_msg(s->ctx, &msg);
}

} // namespace

task<> ws_parser::send_text(std::string_view payload) {
    int rv = queue_data_message(state_.get(), WSLAY_TEXT_FRAME,
                                payload.data(), payload.size());
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

task<> ws_parser::send_binary(const void* data, size_t n) {
    int rv = queue_data_message(state_.get(), WSLAY_BINARY_FRAME, data, n);
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

task<> ws_parser::send_ping(std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PING,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

task<> ws_parser::send_pong(std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PONG,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(state_->ctx, &msg);
    if (rv != 0) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

task<> ws_parser::send_close(uint16_t code, std::string_view reason) {
    int rv = wslay_event_queue_close(
        state_->ctx, code,
        reinterpret_cast<const uint8_t*>(reason.data()),
        reason.size());
    if (rv != 0 && rv != WSLAY_ERR_NO_MORE_MSG) {
        throw ws_error(rv);
    }
    co_await pump_writes();
}

task<> ws_parser::close() {
    if (!state_) {
        co_return;
    }
    if (!wslay_event_get_close_sent(state_->ctx)) {
        co_await send_close(1000, "");
    }
    // Drain incoming traffic until peer sends close or transport EOFs.
    while (wslay_event_get_read_enabled(state_->ctx)
           && !wslay_event_get_close_received(state_->ctx)) {
        bool ok = co_await pump_reads();
        if (!ok) {
            break;
        }
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

task<> ws_parser::handshake() {
    if (!state_ || !state_->is_client) {
        throw ws_error(WSLAY_ERR_INVALID_ARGUMENT,
                       "handshake() called on non-client ws_parser");
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
            if (i != 0) {
                req += ", ";
            }
            req += client_subprotocols_[i];
        }
        req += "\r\n";
    }
#if COTAMER_HAVE_ZLIB
    if (unsigned(client_offer_options_ & ws_connection_options::permessage_deflate)) {
        // Offer with `client_max_window_bits` so a server that wants to
        // restrict our window can do so by echoing a value.
        req += "Sec-WebSocket-Extensions: permessage-deflate; "
               "client_max_window_bits";
        if (unsigned(client_offer_options_
                     & ws_connection_options::server_no_context_takeover)) {
            req += "; server_no_context_takeover";
        }
        if (unsigned(client_offer_options_
                     & ws_connection_options::client_no_context_takeover)) {
            req += "; client_no_context_takeover";
        }
        req += "\r\n";
    }
#endif
    req += "\r\n";

    iovec iov{ req.data(), req.size() };
    co_await stream_->sendv(&iov, 1, MSG_WAITALL);

    // Parse the 101 response with http_parser using the same underlying
    // stream we've been writing to.
    http_parser hp(std::move(stream_), http_parser::client);
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

    // Header iteration yields one element per header line; the same field
    // name can recur. Combine multi-instance fields per RFC 9110 §5.3:
    // list-valued fields concatenate with `,`, single-valued must appear
    // at most once.
    bool saw_upgrade = false, saw_connection = false;
    std::string accept;
    std::string extensions;
    for (auto it = resp.header_begin(); it != resp.header_end(); ++it) {
        if (it.name_ieq("Upgrade")) {
            saw_upgrade = saw_upgrade
                || strings::http_value_list(it.value())
                       .icontains("websocket");
        } else if (it.name_ieq("Connection")) {
            saw_connection = saw_connection
                || strings::http_value_list(it.value())
                       .icontains("upgrade");
        } else if (it.name_ieq("Sec-WebSocket-Accept")) {
            // Must appear exactly once per RFC 6455 §4.1.
            if (accept.empty()) {
                accept.assign(it.value().data(), it.value().size());
            }
        } else if (it.name_ieq("Sec-WebSocket-Extensions")) {
            if (!extensions.empty()) {
                extensions += ", ";
            }
            extensions.append(it.value().data(), it.value().size());
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

#if COTAMER_HAVE_ZLIB
    if (!extensions.empty()) {
        ws_connection_options opts = {};
        parse_extension_header(extensions, opts);
        if (unsigned(opts & ws_connection_options::permessage_deflate)) {
            if (!unsigned(client_offer_options_
                          & ws_connection_options::permessage_deflate)) {
                throw ws_error(WSLAY_ERR_PROTO,
                               "ws handshake: server enabled an extension we "
                               "did not offer");
            }
            if (unsigned(opts & ws_connection_options::unknown_option)) {
                throw ws_error(WSLAY_ERR_PROTO,
                               "ws handshake: server selected unsupported "
                               "permessage-deflate parameters");
            }
            enable_permessage_deflate(state_.get(), opts);
        }
    }
#else
    (void) extensions;
#endif

    // Stash any bytes that arrived alongside the 101 (a server might have
    // piggybacked a frame on the same TCP segment) into wslay's recv buffer
    // before we let it pull more from the transport.
    stream_ = hp.take_stream();
    std::string residual = std::move(hp.receive_buffer());
    if (!residual.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(residual.data());
        state_->recv_buf.insert(state_->recv_buf.end(),
                                p, p + residual.size());
    }
}


// --- server upgrade --------------------------------------------------------

bool is_ws_upgrade_request(const http_message& req) {
    if (req.method() != HTTP_GET) {
        return false;
    }
    if (req.http_major() < 1
        || (req.http_major() == 1 && req.http_minor() < 1)) {
        return false;
    }
    auto upg = req.header("upgrade");
    if (upg.empty()
        || !strings::http_value_list(upg).icontains("websocket")) {
        return false;
    }
    auto conn = req.header("connection");
    if (conn.empty()
        || !strings::http_value_list(conn).icontains("upgrade")) {
        return false;
    }
    if (req.header("sec-websocket-key").empty()) {
        return false;
    }
    // Joins duplicate occurrences with ", "; "13, 13" naturally fails.
    if (req.header("sec-websocket-version") != "13") {
        return false;
    }
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

task<ws_parser> ws_upgrade(http_parser&& hp, const http_message& req,
                           std::vector<std::string> subprotocols,
                           ws_connection_options accept_options) {
    if (!is_ws_upgrade_request(req)) {
        co_await write_400(hp, "Bad WebSocket upgrade request");
        throw ws_error(WSLAY_ERR_PROTO, "bad ws upgrade request");
    }

    std::string key = req.header("sec-websocket-key");
    std::string accept = ws_compute_accept(key);

    // Pick a subprotocol if both sides support one.
    std::string chosen_subprotocol;
    if (!subprotocols.empty()) {
        auto offered = req.header("sec-websocket-protocol");
        if (!offered.empty()) {
            for (auto tok : strings::http_value_list(offered)) {
                for (auto& sup : subprotocols) {
                    if (tok == sup) {
                        chosen_subprotocol = sup;
                        break;
                    }
                }
                if (!chosen_subprotocol.empty()) {
                    break;
                }
            }
        }
    }

    // Decide whether to negotiate permessage-deflate.
    ws_connection_options conn_options = {};
#if COTAMER_HAVE_ZLIB
    if (unsigned(accept_options & ws_connection_options::permessage_deflate)) {
        auto offered = req.header("sec-websocket-extensions");
        if (!offered.empty()) {
            parse_extension_header(offered, conn_options);
            if (unsigned(conn_options & ws_connection_options::unknown_option)) {
                // Decline to negotiate if the offer specifies a parameter
                // we cannot honor.
                conn_options = {};
            } else if (unsigned(conn_options & ws_connection_options::permessage_deflate)) {
                // OR in any server-forced no-context-takeover preferences.
                conn_options |= accept_options
                    & (ws_connection_options::server_no_context_takeover
                       | ws_connection_options::client_no_context_takeover);
            }
        }
    }
#else
    (void) accept_options;
#endif

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
    if (unsigned(conn_options & ws_connection_options::permessage_deflate)) {
        res += "Sec-WebSocket-Extensions: permessage-deflate";
        if (unsigned(conn_options & ws_connection_options::server_no_context_takeover)) {
            res += "; server_no_context_takeover";
        }
        if (unsigned(conn_options & ws_connection_options::client_no_context_takeover)) {
            res += "; client_no_context_takeover";
        }
        res += "\r\n";
    }
    res += "\r\n";

    // Recover any bytes that arrived past the end of the upgrade headers
    // (e.g. a frame piggybacked on the same TCP segment). They belong to
    // the WebSocket protocol now.
    auto strm = hp.take_stream();
    co_await strm->send(res.data(), res.size(), MSG_WAITALL);

    co_return ws_parser::wrap_server(std::move(strm), std::move(hp.receive_buffer()),
                                     conn_options);
}

} // namespace cotamer
