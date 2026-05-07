#pragma once
#include "cotamer/cotamer.hh"
#include "cotamer/io.hh"
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
struct iovec;

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
    // tls_stream is reference-counted, like cotamer::fd: copying produces a
    // second handle to the same TLS session over the same underlying socket.
    // The session and fd are torn down when the last handle goes away.
    tls_stream() = default;
    tls_stream(tls_stream&&) noexcept = default;
    tls_stream& operator=(tls_stream&&) noexcept = default;
    tls_stream(const tls_stream&) = default;
    tls_stream& operator=(const tls_stream&) = default;
    ~tls_stream() = default;

    // Free-function I/O primitives (in namespace cotamer). These match the
    // signatures of the fd-based primitives in `io.hh` — including taking
    // the stream by value, so the coroutine frame's refcount bump keeps the
    // session alive even if the caller's handle goes out of scope.
    friend task<ioresult> recv_once(tls_stream, void*, size_t);
    friend task<ioresult> recv(tls_stream, void*, size_t);
    friend task<ioresult> send_once(tls_stream, const void*, size_t);
    friend task<ioresult> send(tls_stream, const void*, size_t);
    friend task<ioresult> sendv(tls_stream, const struct iovec*, size_t);

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

    const fd& underlying() const noexcept;
    std::string_view alpn() const noexcept;           // negotiated protocol, or empty

    explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

// Convenience: resolve address, connect, handshake.
task<tls_stream> tls_connect(std::string address,
                             std::string hostname,
                             const tls_context& ctx);

// Convenience: accept next incoming connection, handshake.
task<tls_stream> tls_accept(const fd& listen_fd, const tls_context& ctx);

// Free-function I/O on `tls_stream`. Declared in namespace `cotamer`, so
// unqualified-call code (and ADL) finds them when given a `tls_stream`.
// Stream is captured by value: the refcount bump keeps the TLS session
// (and its underlying socket) alive across the coroutine's awaits.
task<ioresult> recv_once(tls_stream s, void* buf, size_t n);
task<ioresult> recv(tls_stream s, void* buf, size_t n);
task<ioresult> send_once(tls_stream s, const void* buf, size_t n);
task<ioresult> send(tls_stream s, const void* buf, size_t n);
task<ioresult> sendv(tls_stream s, const struct iovec* iov, size_t iovcnt);

// Aliases for the read/write naming family.
inline task<ioresult> read_once(tls_stream s, void* buf, size_t n) {
    return recv_once(std::move(s), buf, n);
}
inline task<ioresult> read(tls_stream s, void* buf, size_t n) {
    return recv(std::move(s), buf, n);
}
inline task<ioresult> write_once(tls_stream s, const void* buf, size_t n) {
    return send_once(std::move(s), buf, n);
}
inline task<ioresult> write(tls_stream s, const void* buf, size_t n) {
    return send(std::move(s), buf, n);
}
inline task<ioresult> writev(tls_stream s, const struct iovec* iov, size_t iovcnt) {
    return sendv(std::move(s), iov, iovcnt);
}

} // namespace cotamer
