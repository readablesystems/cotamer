#include "cotamer/cotamer.hh"
#include "cotamer/tls.hh"
#include "cotamer/config.hh"
#if COTAMER_HAVE_LLHTTP
# include "cotamer/http.hh"
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/uio.h>

namespace cot = cotamer;
using namespace std::chrono_literals;

namespace {

// Paths to a self-signed test cert generated once for the whole test run.
std::string test_cert_path;
std::string test_key_path;

void make_self_signed() {
    auto tmp = std::filesystem::temp_directory_path() / "cot-test-tls";
    std::filesystem::create_directories(tmp);
    test_cert_path = (tmp / "cert.pem").string();
    test_key_path = (tmp / "key.pem").string();

    // Self-signed RSA key with localhost CN and SAN, valid 2 days.
    std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -days 2 "
        "-keyout '" + test_key_path + "' "
        "-out '"   + test_cert_path + "' "
        "-subj '/CN=localhost' "
        "-addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' "
        ">/dev/null 2>&1";
    int r = std::system(cmd.c_str());
    if (r != 0) {
        std::cerr << "failed to generate test cert (openssl missing?)\n";
        std::exit(2);
    }
}

uint16_t unique_port() {
    static uint16_t next = 29100;
    return next++;
}

} // namespace


// TEST: loopback client↔server handshake, then read/write.
cot::task<> test_handshake_and_io() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    auto server = [&]() -> cot::task<> {
        auto sctx = cot::tls_context::make_server(test_cert_path,
                                                  test_key_path);
        auto lfd = co_await cot::tcp_listen(addr);
        auto s = co_await cot::tls_accept(lfd, sctx);

        char buf[64] = {};
        size_t n = co_await s.read(buf, sizeof(buf));
        assert(n > 0);
        assert(std::string(buf, n) == "ping");
        co_await s.write("pong", 4);
        co_await s.shutdown();
    };
    server().detach();

    co_await cot::after(5ms); // let server start listening

    auto cctx = cot::tls_context::make_client();
    cctx.add_ca_file(test_cert_path);  // trust the self-signed cert

    auto s = co_await cot::tls_connect(addr, "localhost", cctx);
    co_await s.write("ping", 4);
    char buf[64] = {};
    size_t n = co_await s.read(buf, sizeof(buf));
    assert(n == 4);
    assert(std::string(buf, n) == "pong");
    std::cerr << "handshake_and_io: ok\n";
}


// TEST: verification REQUIRED with no trust for self-signed cert → handshake
// throws tls_error on the client side.
cot::task<> test_bad_cert() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    auto server = [&]() -> cot::task<> {
        auto sctx = cot::tls_context::make_server(test_cert_path,
                                                  test_key_path);
        auto lfd = co_await cot::tcp_listen(addr);
        try {
            co_await cot::tls_accept(lfd, sctx);
            std::cerr << "bad_cert: server handshake unexpectedly ok\n";
        } catch (const cot::tls_error& e) {
            std::cerr << "bad_cert: server saw " << e.what() << "\n";
        } catch (...) {
            std::cerr << "bad_cert: server saw unknown exception\n";
        }
    };
    auto server_task = server();

    co_await cot::after(5ms);

    // Load a different self-signed cert as the CA so verification fails.
    auto cctx = cot::tls_context::make_client();
    auto tmp = std::filesystem::temp_directory_path() / "cot-test-tls";
    auto unrelated = (tmp / "unrelated.pem").string();
    std::system(("openssl req -x509 -newkey rsa:2048 -nodes -days 2 "
                 "-keyout /dev/null -out '" + unrelated + "' "
                 "-subj '/CN=other' >/dev/null 2>&1").c_str());
    cctx.add_ca_file(unrelated);

    bool threw = false;
    try {
        co_await cot::tls_connect(addr, "localhost", cctx);
    } catch (const cot::tls_error& e) {
        threw = true;
        std::cerr << "bad_cert: rejected (" << e.what() << ")\n";
    }
    assert(threw);

    // Wait for server to observe the disconnect, bounded.
    co_await cot::attempt(std::move(server_task), cot::after(500ms));
}


// TEST: ALPN negotiation.
cot::task<> test_alpn() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    std::string server_alpn, client_alpn;

    auto server = [&]() -> cot::task<> {
        auto sctx = cot::tls_context::make_server(test_cert_path,
                                                  test_key_path);
        sctx.set_alpn({"h2", "http/1.1"});
        auto lfd = co_await cot::tcp_listen(addr);
        auto s = co_await cot::tls_accept(lfd, sctx);
        server_alpn = std::string(s.alpn());
        co_await s.shutdown();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cctx = cot::tls_context::make_client();
    cctx.add_ca_file(test_cert_path);
    cctx.set_alpn({"h2", "http/1.1"});

    auto s = co_await cot::tls_connect(addr, "localhost", cctx);
    client_alpn = std::string(s.alpn());

    // Give the server a moment to capture its side.
    co_await cot::after(5ms);

    assert(client_alpn == "h2");
    assert(server_alpn == "h2");
    std::cerr << "alpn: negotiated \"" << client_alpn << "\"\n";
}


// TEST: drive the I/O free-function family (recv_once, send, sendv) directly
// against a tls_stream — the same primitives the http_parser/websocket
// templates dispatch through.
cot::task<> test_free_functions() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    auto server = [&]() -> cot::task<> {
        auto sctx = cot::tls_context::make_server(test_cert_path,
                                                  test_key_path);
        auto lfd = co_await cot::tcp_listen(addr);
        auto s = co_await cot::tls_accept(lfd, sctx);

        char buf[64] = {};
        auto nr = co_await cot::recv_once(s, buf, sizeof(buf));
        assert(nr && *nr == 5);
        assert(std::string(buf, *nr) == "PING\n");

        // sendv: gather across two iovecs.
        struct iovec iov[2];
        iov[0] = {(void*) "PO", 2};
        iov[1] = {(void*) "NG\n", 3};
        auto nw = co_await cot::sendv(s, iov, 2);
        assert(nw && *nw == 5);
    };
    server().detach();

    co_await cot::after(5ms);

    auto cctx = cot::tls_context::make_client();
    cctx.add_ca_file(test_cert_path);
    auto s = co_await cot::tls_connect(addr, "localhost", cctx);

    auto sw = co_await cot::send(s, "PING\n", 5);
    assert(sw && *sw == 5);

    char buf[64] = {};
    auto rr = co_await cot::recv(s, buf, 5);
    assert(rr && *rr == 5);
    assert(std::string(buf, 5) == "PONG\n");
    std::cerr << "free_functions: ok\n";
}


#if COTAMER_HAVE_LLHTTP
// TEST: drive http_parser end-to-end over a tls_stream. This exercises both
// the new templated http_parser API and the cotamer::recv_once / cotamer::sendv
// free functions for tls_stream.
cot::task<> test_http_over_tls() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    auto server = [&]() -> cot::task<> {
        auto sctx = cot::tls_context::make_server(test_cert_path,
                                                  test_key_path);
        auto lfd = co_await cot::tcp_listen(addr);
        auto s = co_await cot::tls_accept(lfd, sctx);

        cot::http_parser hp(cot::http_parser::server);
        auto req = co_await hp.receive(s);
        assert(hp.ok());
        assert(req.method() == HTTP_GET);
        assert(req.path() == "/hi");

        cot::http_message res;
        res.status_code(200)
            .header("Content-Type", "text/plain")
            .body("hello over tls\n");
        co_await hp.send_response(s, std::move(res));
    };
    server().detach();

    co_await cot::after(5ms);

    auto cctx = cot::tls_context::make_client();
    cctx.add_ca_file(test_cert_path);
    auto s = co_await cot::tls_connect(addr, "localhost", cctx);

    cot::http_parser hp(cot::http_parser::client, "localhost");
    cot::http_message req(HTTP_GET, "/hi");
    auto ticket = co_await hp.send_request(s, std::move(req));
    auto res = co_await hp.receive(s, std::move(ticket));
    assert(hp.ok());
    assert(res.status_code() == 200);
    assert(res.body() == "hello over tls\n");
    std::cerr << "http_over_tls: ok\n";
}
#endif


int main(int argc, char* argv[]) {
    cot::set_clock(cot::clock::real_time);
    make_self_signed();

    int ran = 0;
    auto run = [&](const char* name, auto fn) {
        bool found = argc == 1;
        for (int i = 1; !found && i < argc; ++i) {
            found = std::strcmp(name, argv[i]) == 0;
        }
        if (!found) return;
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        fn().detach();
        cot::loop();
        cot::reset();
    };

    run("handshake_and_io", test_handshake_and_io);
    run("bad_cert", test_bad_cert);
    run("alpn", test_alpn);
    run("free_functions", test_free_functions);
#if COTAMER_HAVE_LLHTTP
    run("http_over_tls", test_http_over_tls);
#endif

    if (ran == 0) {
        std::cerr << "No matching tests\n";
        return 1;
    }
    std::cerr << "*** done ***\n";
    return 0;
}
