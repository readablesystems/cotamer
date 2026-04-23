#pragma once
#include "cotamer/cotamer.hh"
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// cotamer/tls.hh
//    Coroutine-driven TLS streams built on mbedtls.
//
//    A `tls_context` holds a role (client or server), CA chain, optional
//    certificate+key, ALPN list, and verification mode. Contexts are meant to
//    be long-lived; each connection gets its own `tls_stream` built from a
//    context. The context must outlive every `tls_stream` built from it.
//
//    A `tls_stream` is an owning handle around a cotamer `fd` plus an mbedtls
//    SSL context. Its `read`, `write`, `handshake`, and `shutdown` methods are
//    coroutines that suspend on the underlying fd via `readable`/`writable`
//    whenever mbedtls signals WANT_READ/WANT_WRITE.

namespace cotamer {

enum class tls_verify { none, optional, required };

// Category for mbedtls error codes. Positive integer values of
// std::error_code::value() correspond to `-error_code` in mbedtls conventions
// (mbedtls errors are negative).
const std::error_category& tls_error_category() noexcept;

class tls_error : public std::system_error {
public:
    explicit tls_error(int mbedtls_err);
    int mbedtls_error() const noexcept { return mbedtls_err_; }
private:
    int mbedtls_err_;
};

class tls_stream;

class tls_context {
public:
    // Client context. Trust anchors are loaded lazily from the system bundle
    // on first handshake unless add_ca_* or use_system_cas is called first.
    // Default verify mode is `required`.
    static tls_context make_client();

    // Server context. `cert_path` and `key_path` must point at PEM files.
    // Default verify mode is `none`.
    static tls_context make_server(std::string cert_path,
                                   std::string key_path);

    tls_context(tls_context&&) noexcept;
    tls_context& operator=(tls_context&&) noexcept;
    tls_context(const tls_context&) = delete;
    tls_context& operator=(const tls_context&) = delete;
    ~tls_context();

    void add_ca_file(const std::string& path);
    void add_ca_path(const std::string& path);       // hash-indexed directory
    void use_system_cas();                            // force system bundle load now

    void set_verify(tls_verify v);
    void set_alpn(std::vector<std::string> protos);

    struct impl;
    impl* get() const noexcept { return impl_.get(); }

private:
    tls_context();
    std::unique_ptr<impl> impl_;
};

class tls_stream {
public:
    tls_stream() = default;
    tls_stream(tls_stream&&) noexcept;
    tls_stream& operator=(tls_stream&&) noexcept;
    tls_stream(const tls_stream&) = delete;
    tls_stream& operator=(const tls_stream&) = delete;
    ~tls_stream();

    // Wrap an already-connected fd in a TLS client stream. `hostname` is used
    // for SNI and certificate verification. Handshake is not performed;
    // call `handshake()`.
    static tls_stream wrap_client(fd f,
                                  const tls_context& ctx,
                                  std::string hostname);

    // Wrap an already-accepted fd in a TLS server stream. Handshake not yet
    // performed; call `handshake()`.
    static tls_stream wrap_server(fd f, const tls_context& ctx);

    task<> handshake();
    task<size_t> read(void* buf, size_t n);
    task<size_t> write(const void* buf, size_t n);
    task<> shutdown();                                // sends close_notify

    const fd& underlying() const noexcept { return f_; }
    std::string_view alpn() const noexcept;           // negotiated protocol, or empty

    explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    fd f_;
};

// Convenience: resolve address, connect, handshake.
task<tls_stream> tls_connect(std::string address,
                             std::string hostname,
                             const tls_context& ctx);

// Convenience: accept next incoming connection, handshake.
task<tls_stream> tls_accept(const fd& listen_fd, const tls_context& ctx);

} // namespace cotamer
