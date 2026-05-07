#include "cotamer/config.hh"
#include "cotamer/tls.hh"
#include "cotamer/io.hh"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <sys/types.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <psa/crypto.h>

namespace cotamer {

// --- error category ---------------------------------------------------------

namespace {

class tls_error_category_impl : public std::error_category {
public:
    const char* name() const noexcept override { return "mbedtls"; }
    std::string message(int value) const override {
        char buf[256] = {};
        // Category stores `-mbedtls_error` so it's positive.
        mbedtls_strerror(-value, buf, sizeof(buf));
        if (buf[0] == '\0') {
            return "mbedtls error " + std::to_string(-value);
        }
        return buf;
    }
};

const tls_error_category_impl& tls_error_category_singleton() {
    static tls_error_category_impl c;
    return c;
}

} // namespace

const std::error_category& tls_error_category() noexcept {
    return tls_error_category_singleton();
}

tls_error::tls_error(int mbedtls_err)
    : std::system_error(std::error_code(-mbedtls_err, tls_error_category_singleton())),
      mbedtls_err_(mbedtls_err) {
}

// --- PSA init ---------------------------------------------------------------

namespace {

// On macOS and BSDs there is no MSG_NOSIGNAL; set SO_NOSIGPIPE on the socket
// so writes to a half-closed peer return EPIPE instead of killing the process.
void suppress_sigpipe(int fileno) {
#ifndef MSG_NOSIGNAL
    int one = 1;
    ::setsockopt(fileno, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void) fileno;
#endif
}

void ensure_psa_crypto_init() {
    static std::once_flag once;
    static int status = 0;
    std::call_once(once, []() {
        status = psa_crypto_init();
    });
    if (status != 0) {
        throw std::runtime_error("psa_crypto_init failed: "
                                 + std::to_string(status));
    }
}

} // namespace

// --- system CA resolution ---------------------------------------------------

namespace {

// Probe order: env vars, then common platform paths (borrowed from curl).
bool try_load_system_cas(mbedtls_x509_crt* chain) {
    if (const char* p = ::getenv("SSL_CERT_FILE")) {
        int r = mbedtls_x509_crt_parse_file(chain, p);
        if (r >= 0) {
            return true;
        }
    }
    if (const char* p = ::getenv("SSL_CERT_DIR")) {
        int r = mbedtls_x509_crt_parse_path(chain, p);
        if (r >= 0) {
            return true;
        }
    }
    static const char* const files[] = {
        "/etc/ssl/cert.pem",                   // macOS, Alpine
        "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/Fedora
        "/etc/ssl/ca-bundle.pem",              // OpenSUSE
        "/etc/pki/tls/cacert.pem",
        nullptr
    };
    for (int i = 0; files[i]; ++i) {
        int r = mbedtls_x509_crt_parse_file(chain, files[i]);
        if (r >= 0) {
            return true;
        }
    }
    static const char* const dirs[] = {
        "/etc/ssl/certs",
        "/etc/pki/tls/certs",
        nullptr
    };
    for (int i = 0; dirs[i]; ++i) {
        int r = mbedtls_x509_crt_parse_path(chain, dirs[i]);
        if (r >= 0) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- tls_context ------------------------------------------------------------

struct tls_context::impl {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca_chain;
    mbedtls_x509_crt own_cert;
    mbedtls_pk_context pk;
    bool is_client;
    bool has_cert = false;
    bool ca_loaded = false;
    bool ca_attached = false;

    std::vector<std::string> alpn_storage;
    std::vector<const char*> alpn_ptrs;

    impl(bool client) : is_client(client) {
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&ca_chain);
        mbedtls_x509_crt_init(&own_cert);
        mbedtls_pk_init(&pk);
    }
    ~impl() {
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca_chain);
        mbedtls_x509_crt_free(&own_cert);
        mbedtls_pk_free(&pk);
    }
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    // Called before each setup of an ssl context from this config.
    void ensure_ready() {
        if (is_client && !ca_loaded) {
            if (!try_load_system_cas(&ca_chain)) {
                throw std::runtime_error(
                    "tls_context: no trust anchors found; "
                    "call add_ca_file() or use_system_cas()");
            }
            ca_loaded = true;
        }
        if (is_client && !ca_attached) {
            mbedtls_ssl_conf_ca_chain(&conf, &ca_chain, nullptr);
            ca_attached = true;
        }
    }
};

tls_context::tls_context() = default;
tls_context::tls_context(tls_context&&) noexcept = default;
tls_context& tls_context::operator=(tls_context&&) noexcept = default;
tls_context::~tls_context() = default;

tls_context tls_context::make_client() {
    ensure_psa_crypto_init();
    tls_context ctx;
    ctx.impl_ = std::make_unique<impl>(true);
    int r = mbedtls_ssl_config_defaults(&ctx.impl_->conf,
                                        MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) throw tls_error(r);
    mbedtls_ssl_conf_authmode(&ctx.impl_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    return ctx;
}

tls_context tls_context::make_server(std::string cert_path,
                                     std::string key_path) {
    ensure_psa_crypto_init();
    tls_context ctx;
    ctx.impl_ = std::make_unique<impl>(false);
    int r = mbedtls_ssl_config_defaults(&ctx.impl_->conf,
                                        MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) throw tls_error(r);
    mbedtls_ssl_conf_authmode(&ctx.impl_->conf, MBEDTLS_SSL_VERIFY_NONE);

    r = mbedtls_x509_crt_parse_file(&ctx.impl_->own_cert, cert_path.c_str());
    if (r != 0) throw tls_error(r);
    r = mbedtls_pk_parse_keyfile(&ctx.impl_->pk, key_path.c_str(), nullptr);
    if (r != 0) throw tls_error(r);
    r = mbedtls_ssl_conf_own_cert(&ctx.impl_->conf,
                                  &ctx.impl_->own_cert,
                                  &ctx.impl_->pk);
    if (r != 0) throw tls_error(r);
    ctx.impl_->has_cert = true;
    return ctx;
}

void tls_context::add_ca_file(const std::string& path) {
    int r = mbedtls_x509_crt_parse_file(&impl_->ca_chain, path.c_str());
    if (r < 0) throw tls_error(r);
    impl_->ca_loaded = true;
}

void tls_context::add_ca_path(const std::string& path) {
    int r = mbedtls_x509_crt_parse_path(&impl_->ca_chain, path.c_str());
    if (r < 0) throw tls_error(r);
    impl_->ca_loaded = true;
}

void tls_context::use_system_cas() {
    if (!try_load_system_cas(&impl_->ca_chain)) {
        throw std::runtime_error(
            "tls_context::use_system_cas: no trust anchors found");
    }
    impl_->ca_loaded = true;
}

void tls_context::set_verify(tls_verify v) {
    int mode = MBEDTLS_SSL_VERIFY_NONE;
    switch (v) {
    case tls_verify::none:     mode = MBEDTLS_SSL_VERIFY_NONE; break;
    case tls_verify::optional: mode = MBEDTLS_SSL_VERIFY_OPTIONAL; break;
    case tls_verify::required: mode = MBEDTLS_SSL_VERIFY_REQUIRED; break;
    }
    mbedtls_ssl_conf_authmode(&impl_->conf, mode);
}

void tls_context::set_alpn(std::vector<std::string> protos) {
    impl_->alpn_storage = std::move(protos);
    impl_->alpn_ptrs.clear();
    impl_->alpn_ptrs.reserve(impl_->alpn_storage.size() + 1);
    for (auto& s : impl_->alpn_storage) {
        impl_->alpn_ptrs.push_back(s.c_str());
    }
    impl_->alpn_ptrs.push_back(nullptr);
    int r = mbedtls_ssl_conf_alpn_protocols(&impl_->conf,
                                            impl_->alpn_ptrs.data());
    if (r != 0) throw tls_error(r);
}

// --- tls_stream -------------------------------------------------------------

struct tls_stream::impl {
    mbedtls_ssl_context ssl;
    fd f;

    explicit impl(fd f_) : f(std::move(f_)) { mbedtls_ssl_init(&ssl); }
    ~impl() { mbedtls_ssl_free(&ssl); }
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
        auto* s = static_cast<impl*>(ctx);
#ifdef MSG_NOSIGNAL
        ssize_t r = ::send(s->f.fileno(), buf, len, MSG_NOSIGNAL);
#else
        ssize_t r = ::send(s->f.fileno(), buf, len, 0);
#endif
        if (r >= 0) return static_cast<int>(r);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
        auto* s = static_cast<impl*>(ctx);
        ssize_t r = ::recv(s->f.fileno(), buf, len, 0);
        if (r > 0) return static_cast<int>(r);
        if (r == 0) return 0; // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
};

const fd& tls_stream::underlying() const noexcept {
    return impl_->f;
}

tls_stream tls_stream::wrap_client(fd f,
                                   const tls_context& ctx,
                                   std::string hostname) {
    ensure_psa_crypto_init();
    if (!ctx.get() || !ctx.get()->is_client) {
        throw std::invalid_argument("tls_stream::wrap_client: need client context");
    }
    ctx.get()->ensure_ready();

    tls_stream s;
    s.impl_ = std::make_shared<impl>(std::move(f));
    suppress_sigpipe(s.impl_->f.fileno());

    int r = mbedtls_ssl_setup(&s.impl_->ssl, &ctx.get()->conf);
    if (r != 0) throw tls_error(r);
    r = mbedtls_ssl_set_hostname(&s.impl_->ssl, hostname.c_str());
    if (r != 0) throw tls_error(r);
    mbedtls_ssl_set_bio(&s.impl_->ssl, s.impl_.get(),
                        &impl::bio_send, &impl::bio_recv, nullptr);
    return s;
}

tls_stream tls_stream::wrap_server(fd f, const tls_context& ctx) {
    ensure_psa_crypto_init();
    if (!ctx.get() || ctx.get()->is_client) {
        throw std::invalid_argument("tls_stream::wrap_server: need server context");
    }
    ctx.get()->ensure_ready();

    tls_stream s;
    s.impl_ = std::make_shared<impl>(std::move(f));
    suppress_sigpipe(s.impl_->f.fileno());

    int r = mbedtls_ssl_setup(&s.impl_->ssl, &ctx.get()->conf);
    if (r != 0) throw tls_error(r);
    mbedtls_ssl_set_bio(&s.impl_->ssl, s.impl_.get(),
                        &impl::bio_send, &impl::bio_recv, nullptr);
    return s;
}

task<> tls_stream::handshake() {
    while (true) {
        int r = mbedtls_ssl_handshake(&impl_->ssl);
        if (r == 0) {
            co_return;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(impl_->f);
        } else {
            throw tls_error(r);
        }
    }
}

task<size_t> tls_stream::read(void* buf, size_t n) {
    while (true) {
        int r = mbedtls_ssl_read(&impl_->ssl,
                                 static_cast<unsigned char*>(buf), n);
        if (r >= 0) {
            co_return static_cast<size_t>(r);
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            co_return 0;
        } else {
            throw tls_error(r);
        }
    }
}

task<size_t> tls_stream::write(const void* buf, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(buf);
    size_t written = 0;
    while (written < n) {
        int r = mbedtls_ssl_write(&impl_->ssl, p + written, n - written);
        if (r > 0) {
            written += static_cast<size_t>(r);
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(impl_->f);
        } else {
            throw tls_error(r);
        }
    }
    co_return written;
}

task<> tls_stream::shutdown() {
    while (true) {
        int r = mbedtls_ssl_close_notify(&impl_->ssl);
        if (r == 0) {
            co_return;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(impl_->f);
        } else {
            // Unusual but not typically fatal for shutdown. Propagate.
            throw tls_error(r);
        }
    }
}

std::string_view tls_stream::alpn() const noexcept {
    if (!impl_) return {};
    const char* p = mbedtls_ssl_get_alpn_protocol(&impl_->ssl);
    return p ? std::string_view(p) : std::string_view{};
}

// --- free-function I/O primitives ------------------------------------------
//
// These mirror `cotamer::recv_once`, `recv`, `send_once`, `send`, `sendv`
// from `io.hh`, but operate on a `tls_stream`. They translate mbedtls
// errors into `std::error_code` (via `tls_error_category`) rather than
// throwing, which is what the templated `http_parser` and `basic_ws_stream`
// pipelines expect.

namespace {

inline std::error_code tls_errc(int mbedtls_err) {
    return std::error_code(-mbedtls_err, tls_error_category_singleton());
}

} // namespace

task<ioresult> recv_once(tls_stream s, void* buf, size_t n) {
    while (true) {
        int r = mbedtls_ssl_read(&s.impl_->ssl,
                                 static_cast<unsigned char*>(buf), n);
        if (r >= 0) {
            co_return static_cast<size_t>(r);
        } else if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            co_return size_t(0);
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(s.impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(s.impl_->f);
        } else {
            co_return std::unexpected(tls_errc(r));
        }
    }
}

task<ioresult> recv(tls_stream s, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t nr = 0;
    while (nr != n) {
        int r = mbedtls_ssl_read(&s.impl_->ssl,
                                 reinterpret_cast<unsigned char*>(p + nr),
                                 n - nr);
        if (r > 0) {
            nr += static_cast<size_t>(r);
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(s.impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(s.impl_->f);
        } else if (nr > 0) {
            break;
        } else {
            co_return std::unexpected(tls_errc(r));
        }
    }
    co_return nr;
}

task<ioresult> send_once(tls_stream s, const void* buf, size_t n) {
    while (true) {
        int r = mbedtls_ssl_write(&s.impl_->ssl,
                                  static_cast<const unsigned char*>(buf), n);
        if (r >= 0) {
            co_return static_cast<size_t>(r);
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(s.impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(s.impl_->f);
        } else {
            co_return std::unexpected(tls_errc(r));
        }
    }
}

task<ioresult> send(tls_stream s, const void* buf, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(buf);
    size_t nw = 0;
    while (nw != n) {
        int r = mbedtls_ssl_write(&s.impl_->ssl, p + nw, n - nw);
        if (r > 0) {
            nw += static_cast<size_t>(r);
        } else if (r == 0) {
            break;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            co_await readable(s.impl_->f);
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            co_await writable(s.impl_->f);
        } else if (nw > 0) {
            break;
        } else {
            co_return std::unexpected(tls_errc(r));
        }
    }
    co_return nw;
}

// TLS has no native gather-write; fall back to per-iovec `send`s. mbedtls
// buffers writes inside its record layer, so a sequence of small writes is
// not nearly as bad as on a raw socket, but callers that care should still
// concatenate on their side.
task<ioresult> sendv(tls_stream s, const struct iovec* iov, size_t iovcnt) {
    size_t total = 0;
    for (size_t i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        auto r = co_await send(s, iov[i].iov_base, iov[i].iov_len);
        if (!r) {
            if (total > 0) {
                co_return total;
            }
            co_return r;
        }
        total += *r;
        if (*r < iov[i].iov_len) {
            co_return total;
        }
    }
    co_return total;
}


// --- convenience ------------------------------------------------------------

task<tls_stream> tls_connect(std::string address,
                             std::string hostname,
                             const tls_context& ctx) {
    auto f = co_await tcp_connect(std::move(address));
    auto s = tls_stream::wrap_client(std::move(f), ctx, std::move(hostname));
    co_await s.handshake();
    co_return std::move(s);
}

task<tls_stream> tls_accept(const fd& listen_fd, const tls_context& ctx) {
    auto f = co_await tcp_accept(listen_fd);
    auto s = tls_stream::wrap_server(std::move(f), ctx);
    co_await s.handshake();
    co_return std::move(s);
}

} // namespace cotamer
