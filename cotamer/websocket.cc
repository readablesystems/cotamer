// -*- mode: c++ -*-
#include "cotamer/websocket.hh"
#include "cotamer/io.hh"

#include <wslay/wslay.h>
#if COTAMER_HAVE_ZLIB
# include <zlib.h>
#endif
#if COTAMER_HAVE_MBEDTLS
# include <psa/crypto.h>
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
    const char* name() const noexcept override { return "wslay"; }
    std::string message(int value) const override {
        return wslay_error_message(value);
    }
};

const ws_error_category_impl& ws_error_category_singleton() {
    static ws_error_category_impl c;
    return c;
}

} // namespace

const std::error_category& ws_error_category() noexcept {
    return ws_error_category_singleton();
}

ws_error::ws_error(int wslay_err)
    : std::system_error(std::error_code(wslay_err, ws_error_category_singleton())) {
}
ws_error::ws_error(int wslay_err, const std::string& what)
    : std::system_error(std::error_code(wslay_err, ws_error_category_singleton()), what) {
}


// --- SHA-1 -----------------------------------------------------------------
//
// Used only by `ws_compute_accept` for the WebSocket handshake. When mbedtls
// is available we defer to it; otherwise fall back to a small reference
// implementation (Steve Reid's, RFC 3174, public domain).

namespace {

#if COTAMER_HAVE_MBEDTLS

void sha1_oneshot(std::string_view a, std::string_view b, uint8_t out[20]) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_1) != PSA_SUCCESS
        || psa_hash_update(&op,
                           reinterpret_cast<const unsigned char*>(a.data()),
                           a.size()) != PSA_SUCCESS
        || psa_hash_update(&op,
                           reinterpret_cast<const unsigned char*>(b.data()),
                           b.size()) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        throw ws_error(WSLAY_ERR_NOMEM, "psa_hash_update failed");
    }
    size_t hash_len = 0;
    if (psa_hash_finish(&op, out, 20, &hash_len) != PSA_SUCCESS
        || hash_len != 20) {
        psa_hash_abort(&op);
        throw ws_error(WSLAY_ERR_NOMEM, "psa_hash_finish failed");
    }
}

#else

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
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
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

void sha1_oneshot(std::string_view a, std::string_view b, uint8_t out[20]) {
    sha1_state s;
    sha1_init(s);
    sha1_update(s, a.data(), a.size());
    sha1_update(s, b.data(), b.size());
    sha1_final(s, out);
}

#endif


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
    uint8_t digest[20];
    sha1_oneshot(key,
                 std::string_view(ws_magic, sizeof(ws_magic) - 1),
                 digest);
    return base64_encode(digest, 20);
}


// --- header utilities ------------------------------------------------------

bool header_contains_ci_token(std::string_view value, std::string_view token) {
    for (auto v : strings::http_header_values(value)) {
        if (strings::ieq(v.name(), token)) {
            return true;
        }
    }
    return false;
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
    std::string recv_buf;
    size_t recv_pos = 0;
    bool recv_eof = false;

    // Outbound bytes wslay has serialized but we haven't yet flushed to the
    // transport.
    std::string send_buf;

    // Completed messages (data + close), enqueued by on_msg_recv_callback
    // and consumed by receive().
    std::deque<std::variant<ws_message, ws_close>> msg_queue;

    // permessage-deflate state. The zlib streams are allocated lazily when
    // negotiation succeeds; `options` carries the negotiated flags
    // (permessage_deflate plus the takeover bits).
    ws_options options = ws_options::none;
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
    s->send_buf.append(reinterpret_cast<const char*>(data), len);
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
            && !!(s->options & ws_options::permessage_deflate)
            && s->inflate_strm) {
            // Inbound = peer's outbound: a server reads what the client sent
            // (so client_no_context_takeover applies), and vice versa.
            ws_options peer_flag = s->is_client
                ? ws_options::server_no_context_takeover
                : ws_options::client_no_context_takeover;
            try {
                auto out = s->inflate_strm->decompress(
                    arg->msg, arg->msg_length,
                    !!(s->options & peer_flag));
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


// --- helpers (anonymous, all non-template) ---------------------------------

namespace {

// Switch a ws_state into permessage-deflate mode. Allocates the zlib
// streams and tells wslay to allow the RSV1 bit on incoming frames.
// `takeover_flags` should carry the negotiated `*_no_context_takeover` bits
// (the `permessage_deflate` bit is added unconditionally by this function).
void enable_permessage_deflate(detail::ws_state* s,
                               ws_options takeover_flags) {
#if COTAMER_HAVE_ZLIB
    s->options = takeover_flags | ws_options::permessage_deflate;
    s->deflate_strm = std::make_unique<detail::deflate_compressor>();
    s->inflate_strm = std::make_unique<detail::inflate_decompressor>();
    wslay_event_config_set_allowed_rsv_bits(s->ctx, WSLAY_RSV1_BIT);
#else
    (void) s;
    (void) takeover_flags;
    throw ws_error(WSLAY_ERR_INVALID_ARGUMENT,
                   "permessage-deflate requested but zlib is not available");
#endif
}

// Parse a Sec-WebSocket-Extensions value, looking for permessage-deflate.
// Returns true if found. On true, sets `takeover` to the negotiated takeover
// bits (subset of {server_no_context_takeover, client_no_context_takeover}).
bool parse_extension_header(std::string_view header,
                            ws_options& takeover,
                            bool& has_unknown_param) {
    takeover = ws_options::none;
    has_unknown_param = false;

    for (auto v : strings::http_header_values(header)) {
        if (!strings::ieq(v.name(), "permessage-deflate")) {
            continue;
        }
        for (auto p : v.params()) {
            if (strings::ieq(p.name, "server_no_context_takeover")) {
                takeover |= ws_options::server_no_context_takeover;
            } else if (strings::ieq(p.name, "client_no_context_takeover")) {
                takeover |= ws_options::client_no_context_takeover;
            } else if (strings::ieq(p.name, "server_max_window_bits")
                       || strings::ieq(p.name, "client_max_window_bits")) {
                // We always use 15-bit windows. The standalone form (no
                // value) is informational; a value other than 15 is a
                // window restriction we don't honor.
                if (!p.value.empty() && p.value != "15") {
                    has_unknown_param = true;
                }
            } else if (!p.name.empty()) {
                has_unknown_param = true;
            }
        }
        return true;
    }
    return false;
}

// Queue a data message, applying permessage-deflate compression if enabled.
// Returns the wslay return code from the queue call.
int queue_data_message(detail::ws_state* s, uint8_t opcode,
                       const void* data, size_t n) {
#if COTAMER_HAVE_ZLIB
    if (!!(s->options & ws_options::permessage_deflate) && s->deflate_strm) {
        // Outbound = our own side: server uses server_no_context_takeover,
        // client uses client_no_context_takeover.
        ws_options own_flag = s->is_client
            ? ws_options::client_no_context_takeover
            : ws_options::server_no_context_takeover;
        auto compressed = s->deflate_strm->compress(
            data, n, !!(s->options & own_flag));
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

std::string make_random_key_impl() {
    uint8_t key[16];
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (size_t i = 0; i < 16; ++i) {
        key[i] = uint8_t(rng() & 0xff);
    }
    return base64_encode(key, 16);
}

} // namespace


// --- ws_stream (non-template) ----------------------------------------------

ws_stream::ws_stream() = default;
ws_stream::ws_stream(ws_stream&&) noexcept = default;
ws_stream& ws_stream::operator=(ws_stream&&) noexcept = default;
ws_stream::~ws_stream() = default;

ws_stream ws_stream::make_client(std::string host, std::string path,
                                 std::vector<std::string> subprotocols,
                                 ws_options opts) {
    ws_stream s;
    s.state_ = std::make_unique<detail::ws_state>();
    detail::ws_state_init(s.state_.get(), /*is_client=*/true);
    s.client_host_ = std::move(host);
    s.client_path_ = std::move(path);
    s.client_subprotocols_ = std::move(subprotocols);
    s.client_options_ = opts;
    return s;
}

ws_stream ws_stream::make_server(std::string residual, ws_options opts) {
    ws_stream s;
    s.state_ = std::make_unique<detail::ws_state>();
    detail::ws_state_init(s.state_.get(), /*is_client=*/false);
    detail::ws_state_set_residual(s.state_.get(), residual);
    if (!!(opts & ws_options::permessage_deflate)) {
        detail::ws_state_enable_deflate(s.state_.get(), opts);
    }
    return s;
}

bool ws_stream::is_open() const noexcept {
    return state_ && detail::ws_state_is_open(state_.get());
}

bool ws_stream::permessage_deflate_negotiated() const noexcept {
    return state_ && detail::ws_state_deflate_negotiated(state_.get());
}


// --- detail::ws_state_* helpers --------------------------------------------

namespace detail {

void ws_state_init(ws_state* s, bool is_client) {
    s->is_client = is_client;
    int rv = is_client
        ? wslay_event_context_client_init(&s->ctx, &ws_event_callbacks, s)
        : wslay_event_context_server_init(&s->ctx, &ws_event_callbacks, s);
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_set_residual(ws_state* s, std::string_view residual) {
    s->recv_buf.append(residual);
}

void ws_state_enable_deflate(ws_state* s, ws_options takeover_flags) {
    enable_permessage_deflate(s, takeover_flags);
}

bool ws_state_pump_writes_step(ws_state* s, std::string& out) {
    out.clear();
    if (!wslay_event_want_write(s->ctx)) {
        // Even if want_write is false, there might be leftover bytes from
        // the last wslay_event_send.
        if (!s->send_buf.empty()) {
            out.swap(s->send_buf);
        }
        return false;
    }
    int rv = wslay_event_send(s->ctx);
    if (rv != 0) {
        throw ws_error(rv);
    }
    if (!s->send_buf.empty()) {
        out.swap(s->send_buf);
    }
    return wslay_event_want_write(s->ctx) || !out.empty();
}

bool ws_state_drain_recv_buffer(ws_state* s) {
    if (!wslay_event_get_read_enabled(s->ctx)) {
        return false;
    }
    if (s->recv_buf.size() <= s->recv_pos) {
        return false;
    }
    int rv = wslay_event_recv(s->ctx);
    if (rv != 0) {
        throw ws_error(rv);
    }
    return true;
}

void ws_state_feed_recv(ws_state* s, const void* data, size_t n) {
    s->recv_buf.append(static_cast<const char*>(data), n);
    int rv = wslay_event_recv(s->ctx);
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_set_recv_eof(ws_state* s) {
    s->recv_eof = true;
    wslay_event_shutdown_read(s->ctx);
}

bool ws_state_pop_message(ws_state* s,
                          std::variant<ws_message, ws_close>& out) {
    if (s->msg_queue.empty()) {
        return false;
    }
    out = std::move(s->msg_queue.front());
    s->msg_queue.pop_front();
    return true;
}

ws_close ws_state_make_close(ws_state* s) {
    ws_close c;
    c.code = wslay_event_get_close_received(s->ctx)
        ? wslay_event_get_status_code_received(s->ctx)
        : WSLAY_CODE_ABNORMAL_CLOSURE;
    return c;
}

bool ws_state_read_done(ws_state* s) {
    return !wslay_event_get_read_enabled(s->ctx) || s->recv_eof;
}

bool ws_state_close_received(ws_state* s) {
    return wslay_event_get_close_received(s->ctx);
}

bool ws_state_close_sent(ws_state* s) {
    return wslay_event_get_close_sent(s->ctx);
}

bool ws_state_is_open(ws_state* s) {
    return wslay_event_get_read_enabled(s->ctx)
        || wslay_event_get_write_enabled(s->ctx);
}

bool ws_state_deflate_negotiated(ws_state* s) {
    return !!(s->options & ws_options::permessage_deflate);
}

void ws_state_queue_text(ws_state* s, std::string_view payload) {
    int rv = queue_data_message(s, WSLAY_TEXT_FRAME,
                                payload.data(), payload.size());
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_queue_binary(ws_state* s, const void* data, size_t n) {
    int rv = queue_data_message(s, WSLAY_BINARY_FRAME, data, n);
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_queue_ping(ws_state* s, std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PING,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(s->ctx, &msg);
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_queue_pong(ws_state* s, std::string_view payload) {
    wslay_event_msg msg = {
        WSLAY_PONG,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()
    };
    int rv = wslay_event_queue_msg(s->ctx, &msg);
    if (rv != 0) {
        throw ws_error(rv);
    }
}

void ws_state_queue_close(ws_state* s, uint16_t code, std::string_view reason) {
    int rv = wslay_event_queue_close(
        s->ctx, code,
        reinterpret_cast<const uint8_t*>(reason.data()),
        reason.size());
    // WSLAY_ERR_NO_MORE_MSG is benign here: a close has already been queued
    // (perhaps by a handler), and queueing a second one is a no-op.
    if (rv != 0 && rv != WSLAY_ERR_NO_MORE_MSG) {
        throw ws_error(rv);
    }
}

bool ws_state_is_client(ws_state* state) {
    return state && state->is_client;
}

std::string ws_make_random_key() {
    return make_random_key_impl();
}

std::string ws_build_client_request(std::string_view host,
                                    std::string_view path,
                                    std::string_view key,
                                    const std::vector<std::string>& subprotocols,
                                    ws_options client_opts) {
    std::string req = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        path, host, key);
    if (!subprotocols.empty()) {
        req += "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < subprotocols.size(); ++i) {
            if (i) req += ", ";
            req += subprotocols[i];
        }
        req += "\r\n";
    }
#if COTAMER_HAVE_ZLIB
    if (!!(client_opts & ws_options::permessage_deflate)) {
        req += "Sec-WebSocket-Extensions: permessage-deflate; "
               "client_max_window_bits\r\n";
    }
#else
    (void) client_opts;
#endif
    req += "\r\n";
    return req;
}

void ws_validate_handshake_response(ws_state* state,
                                    const http_message& resp,
                                    std::string_view key,
                                    ws_options client_opts) {
    bool saw_upgrade = false, saw_connection = false;
    std::string accept;
    std::string extensions;
    for (auto it = resp.header_begin(); it != resp.header_end(); ++it) {
        if (it.name_ieq("Upgrade")) {
            saw_upgrade = strings::ieq(it.value(), "websocket");
        } else if (it.name_ieq("Connection")) {
            saw_connection = header_contains_ci_token(it.value(), "upgrade");
        } else if (it.name_ieq("Sec-WebSocket-Accept")) {
            accept.assign(it.value().data(), it.value().size());
        } else if (it.name_ieq("Sec-WebSocket-Extensions")) {
            extensions.assign(it.value().data(), it.value().size());
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
        ws_options takeover = ws_options::none;
        bool unknown = false;
        if (parse_extension_header(extensions, takeover, unknown)) {
            if (!(client_opts & ws_options::permessage_deflate)) {
                throw ws_error(WSLAY_ERR_PROTO,
                               "ws handshake: server enabled an extension we "
                               "did not offer");
            }
            if (unknown) {
                throw ws_error(WSLAY_ERR_PROTO,
                               "ws handshake: server selected unsupported "
                               "permessage-deflate parameters");
            }
            enable_permessage_deflate(state, takeover);
        }
    }
#else
    (void) state;
    (void) extensions;
    (void) client_opts;
#endif
}

std::string ws_build_server_response(const http_message& req,
                                     const std::vector<std::string>& subprotocols,
                                     ws_options server_opts,
                                     ws_options& negotiated_out) {
    std::string key{req.find_header("sec-websocket-key").value()};
    std::string accept = ws_compute_accept(key);

    // Pick a subprotocol if both sides support one. Subprotocol tokens are
    // case-sensitive per RFC 6455.
    std::string chosen_subprotocol;
    if (!subprotocols.empty()) {
        auto offered = req.find_header("sec-websocket-protocol");
        if (offered != req.header_end()) {
            for (auto v : strings::http_header_values(offered.value())) {
                bool matched = false;
                for (auto& sup : subprotocols) {
                    if (v.name() == sup) {
                        chosen_subprotocol = sup;
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    break;
                }
            }
        }
    }

    negotiated_out = ws_options::none;
#if COTAMER_HAVE_ZLIB
    if (!!(server_opts & ws_options::permessage_deflate)) {
        auto offered = req.find_header("sec-websocket-extensions");
        if (offered != req.header_end()) {
            ws_options takeover = ws_options::none;
            bool unknown = false;
            if (parse_extension_header(offered.value(), takeover, unknown)
                && !unknown) {
                negotiated_out |= ws_options::permessage_deflate | takeover;
            }
        }
    }
#else
    (void) server_opts;
#endif

    // Build the 101 response by hand. http_parser's send_response would add
    // Content-Length and assumes keep-alive semantics that don't apply to
    // an upgrade.
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
    if (!!(negotiated_out & ws_options::permessage_deflate)) {
        res += "Sec-WebSocket-Extensions: permessage-deflate";
        if (!!(negotiated_out & ws_options::server_no_context_takeover)) {
            res += "; server_no_context_takeover";
        }
        if (!!(negotiated_out & ws_options::client_no_context_takeover)) {
            res += "; client_no_context_takeover";
        }
        res += "\r\n";
    }
    res += "\r\n";
    return res;
}

} // namespace detail


// --- server upgrade --------------------------------------------------------

bool is_ws_upgrade_request(const http_message& req) {
    if (req.method() != HTTP_GET) {
        return false;
    }
    if (req.http_major() < 1 || (req.http_major() == 1 && req.http_minor() < 1)) {
        return false;
    }
    auto upg = req.find_header("upgrade");
    if (upg == req.header_end() || !strings::ieq(upg.value(), "websocket")) {
        return false;
    }
    auto conn = req.find_header("connection");
    if (conn == req.header_end()
        || !header_contains_ci_token(conn.value(), "upgrade")) {
        return false;
    }
    auto key = req.find_header("sec-websocket-key");
    if (key == req.header_end() || key.value().empty()) {
        return false;
    }
    auto ver = req.find_header("sec-websocket-version");
    if (ver == req.header_end() || ver.value() != "13") {
        return false;
    }
    return true;
}

} // namespace cotamer
