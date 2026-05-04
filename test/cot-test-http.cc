#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace cot = cotamer;
using namespace std::chrono_literals;

namespace {

uint16_t unique_port() {
    static uint16_t next = 49100;
    return next++;
}

cot::task<cot::fd> connect_pair(std::string addr, cot::event server_ready) {
    co_await server_ready;
    co_return co_await cot::tcp_connect(addr);
}

} // namespace


// Two requests in one TCP segment must come out as two messages.
cot::task<> test_pipelining_one_segment() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event listener_ready;
    cot::event server_done;

    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        listener_ready.trigger();
        auto cfd = co_await cot::tcp_accept(lfd);
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);

        auto req1 = co_await hp.receive();
        assert(hp.ok());
        assert(req1.method() == HTTP_GET);
        assert(req1.url() == "/first");

        auto req2 = co_await hp.receive();
        assert(hp.ok());
        assert(req2.method() == HTTP_GET);
        assert(req2.url() == "/second");

        server_done.trigger();
    };
    server().detach();

    auto cfd = co_await connect_pair(addr, listener_ready);
    std::string raw =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";
    co_await cot::write(cfd, raw.data(), raw.size());

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "pipelining_one_segment: ok\n";
}


// Two requests where each spans multiple reads (we send byte-by-byte).
cot::task<> test_pipelining_byte_by_byte() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event listener_ready;
    cot::event server_done;

    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        listener_ready.trigger();
        auto cfd = co_await cot::tcp_accept(lfd);
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);

        auto req1 = co_await hp.receive();
        assert(hp.ok());
        assert(req1.url() == "/a");

        auto req2 = co_await hp.receive();
        assert(hp.ok());
        assert(req2.url() == "/b");

        server_done.trigger();
    };
    server().detach();

    auto cfd = co_await connect_pair(addr, listener_ready);
    std::string raw =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    for (char c : raw) {
        co_await cot::write(cfd, &c, 1);
    }

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "pipelining_byte_by_byte: ok\n";
}


// Three requests with body bytes in between.
cot::task<> test_pipelining_with_bodies() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event listener_ready;
    cot::event server_done;

    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        listener_ready.trigger();
        auto cfd = co_await cot::tcp_accept(lfd);
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);

        auto req1 = co_await hp.receive();
        assert(hp.ok());
        assert(req1.method() == HTTP_POST);
        assert(req1.url() == "/p1");
        assert(req1.body() == "hello");

        auto req2 = co_await hp.receive();
        assert(hp.ok());
        assert(req2.method() == HTTP_POST);
        assert(req2.url() == "/p2");
        assert(req2.body() == "world!!");

        auto req3 = co_await hp.receive();
        assert(hp.ok());
        assert(req3.method() == HTTP_GET);
        assert(req3.url() == "/g");

        server_done.trigger();
    };
    server().detach();

    auto cfd = co_await connect_pair(addr, listener_ready);
    std::string raw =
        "POST /p1 HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
        "POST /p2 HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nworld!!"
        "GET /g HTTP/1.1\r\nHost: x\r\n\r\n";
    co_await cot::write(cfd, raw.data(), raw.size());

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "pipelining_with_bodies: ok\n";
}


// An upgrade request: the parser should hand back the message with hp.ok()
// true, and any bytes after the headers should be available via
// receive_buffer().
cot::task<> test_upgrade_residual() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event listener_ready;
    cot::event server_done;

    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        listener_ready.trigger();
        auto cfd = co_await cot::tcp_accept(lfd);
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);

        auto req = co_await hp.receive();
        assert(hp.ok());
        assert(req.has_header("upgrade"));
        std::string residual = hp.receive_buffer();
        assert(residual == "FRAME_BYTES");
        server_done.trigger();
    };
    server().detach();

    auto cfd = co_await connect_pair(addr, listener_ready);
    std::string raw =
        "GET /chat HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\nFRAME_BYTES";
    co_await cot::write(cfd, raw.data(), raw.size());

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "upgrade_residual: ok\n";
}


int main(int argc, char* argv[]) {
    cot::set_clock(cot::clock::real_time);

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

    run("pipelining_one_segment", test_pipelining_one_segment);
    run("pipelining_byte_by_byte", test_pipelining_byte_by_byte);
    run("pipelining_with_bodies", test_pipelining_with_bodies);
    run("upgrade_residual", test_upgrade_residual);

    if (ran == 0) {
        std::cerr << "No matching tests\n";
        return 1;
    }
    std::cerr << "*** done ***\n";
    return 0;
}
