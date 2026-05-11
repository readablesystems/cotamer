#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "cotamer/websocket.hh"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace cot = cotamer;
using namespace std::chrono_literals;

namespace {

uint16_t unique_port() {
    static uint16_t next = 39100;
    return next++;
}

cot::task<> server_one(cot::fd cfd) {
    try {
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            co_return;
        }
        // Anything claiming Upgrade gets handed off to ws_upgrade, which
        // writes a 400 itself if the request is malformed.
        if (req.has_header("upgrade")) {
            try {
                auto ws = co_await cot::ws_upgrade(std::move(hp), req);
                while (true) {
                    auto m = co_await ws.receive();
                    if (std::holds_alternative<cot::ws_close>(m)) {
                        break;
                    }
                    auto& msg = std::get<cot::ws_message>(m);
                    if (msg.opcode == cot::ws_opcode::text) {
                        co_await ws.send_text(msg.payload);
                    } else {
                        co_await ws.send_binary(msg.payload);
                    }
                }
                co_await ws.close();
            } catch (const cot::ws_error&) {
                // ws_upgrade already wrote the 400 response.
            }
            co_return;
        }
        cot::http_message res;
        res.status_code(400).body("not a ws upgrade\n");
        co_await hp.send_response(std::move(res));
    } catch (const std::exception& e) {
        std::cerr << "server: exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "server: unknown exception\n";
    }
}

cot::task<> run_server(std::string addr, cot::event done) {
    auto lfd = co_await cot::tcp_listen(addr);
    auto cfd = co_await cot::tcp_accept(lfd);
    co_await server_one(std::move(cfd));
    done.trigger();
}

// Echo server with configurable accept_options.
cot::task<> server_one_deflate(cot::fd cfd, cot::ws_connection_options accept_opts) {
    try {
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);
        auto req = co_await hp.receive();
        if (!hp.ok() || !req.has_header("upgrade")) {
            co_return;
        }
        try {
            auto ws = co_await cot::ws_upgrade(std::move(hp), req, {},
                                               accept_opts);
            while (true) {
                auto m = co_await ws.receive();
                if (std::holds_alternative<cot::ws_close>(m)) {
                    break;
                }
                auto& msg = std::get<cot::ws_message>(m);
                if (msg.opcode == cot::ws_opcode::text) {
                    co_await ws.send_text(msg.payload);
                } else {
                    co_await ws.send_binary(msg.payload);
                }
            }
            co_await ws.close();
        } catch (const cot::ws_error&) {}
    } catch (...) {}
}

} // namespace


// TEST 1: text echo (client → server → client) with a clean close.
cot::task<> test_text_echo() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    run_server(addr, server_done).detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/");
    co_await ws.handshake();

    co_await ws.send_text("hello");

    auto m = co_await ws.receive();
    auto& msg = std::get<cot::ws_message>(m);
    assert(msg.opcode == cot::ws_opcode::text);
    assert(msg.payload == "hello");

    co_await ws.close();

    // Wait for server to clean up. Bound it.
    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "text_echo: ok\n";
}


// TEST 2: large binary message that forces fragmentation/buffering.
cot::task<> test_binary_large() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    run_server(addr, server_done).detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/");
    co_await ws.handshake();

    std::string payload;
    payload.resize(100 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = char(i & 0xff);
    }
    co_await ws.send_binary(payload);

    auto m = co_await ws.receive();
    auto& msg = std::get<cot::ws_message>(m);
    assert(msg.opcode == cot::ws_opcode::binary);
    assert(msg.payload == payload);

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(2000ms));
    std::cerr << "binary_large: ok\n";
}


// TEST 3: the server initiates close with a custom status code; the client
// observes the close on receive() and the close handshake completes.
cot::task<> test_server_close() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        cot::http_parser hp(std::move(cfd), cot::http_parser::server);
        auto req = co_await hp.receive();
        auto ws = co_await cot::ws_upgrade(std::move(hp), req);
        co_await ws.send_close(4321, "bye");
        co_await ws.close();
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/");
    co_await ws.handshake();

    auto m = co_await ws.receive();
    auto* c = std::get_if<cot::ws_close>(&m);
    assert(c);
    assert(c->code == 4321);
    assert(c->reason == "bye");

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "server_close: ok\n";
}


// TEST 4: a malformed handshake (no Sec-WebSocket-Key) should result in a
// 400 from the server.
cot::task<> test_bad_handshake() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        co_await server_one(std::move(cfd));
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    // Send a malformed upgrade request (no Sec-WebSocket-Key).
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    co_await cot::write(cfd, raw.data(), raw.size());

    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf)) {
        auto n = co_await cot::read_once(cfd, buf + total, sizeof(buf) - total);
        if (!n || n == 0) {
            break;
        }
        total += *n;
        std::string_view sv(buf, total);
        if (sv.find("\r\n\r\n") != std::string_view::npos) {
            break;
        }
    }
    std::string_view sv(buf, total);
    assert(sv.starts_with("HTTP/1.1 400"));

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "bad_handshake: ok\n";
}


// TEST: permessage-deflate negotiates, and a highly compressible round-trip
// payload comes back intact.
cot::task<> test_deflate_round_trip() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        co_await server_one_deflate(std::move(cfd),
                                    cot::ws_connection_options::permessage_deflate);
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/",
                                          {}, cot::ws_connection_options::permessage_deflate);
    co_await ws.handshake();
    assert(ws.permessage_deflate_negotiated());

    // Round-trip a moderate, very-compressible message.
    std::string payload(8192, 'A');
    co_await ws.send_text(payload);

    auto m = co_await ws.receive();
    auto& msg = std::get<cot::ws_message>(m);
    assert(msg.opcode == cot::ws_opcode::text);
    assert(msg.payload == payload);

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "deflate_round_trip: ok\n";
}


// TEST: client doesn't offer permessage-deflate; connection still works,
// negotiation flag is false on both sides.
cot::task<> test_deflate_client_off() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        co_await server_one_deflate(std::move(cfd),
                                    cot::ws_connection_options::permessage_deflate);
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/",
                                          {}, cot::ws_connection_options::none);
    co_await ws.handshake();
    assert(!ws.permessage_deflate_negotiated());

    co_await ws.send_text("hello uncompressed");
    auto m = co_await ws.receive();
    assert(std::get<cot::ws_message>(m).payload == "hello uncompressed");

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "deflate_client_off: ok\n";
}


// TEST: client offers but server refuses; connection still works, no deflate.
cot::task<> test_deflate_server_off() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        co_await server_one_deflate(std::move(cfd),
                                    cot::ws_connection_options::none);
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/",
                                          {}, cot::ws_connection_options::permessage_deflate);
    co_await ws.handshake();
    assert(!ws.permessage_deflate_negotiated());

    co_await ws.send_text("uncompressed despite client offer");
    auto m = co_await ws.receive();
    assert(std::get<cot::ws_message>(m).payload
           == "uncompressed despite client offer");

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "deflate_server_off: ok\n";
}


// TEST: large compressible payload through several round-trips, exercising
// context-takeover state across messages.
cot::task<> test_deflate_multi_message() {
    uint16_t port = unique_port();
    auto addr = "127.0.0.1:" + std::to_string(port);

    cot::event server_done;
    auto server = [&]() -> cot::task<> {
        auto lfd = co_await cot::tcp_listen(addr);
        auto cfd = co_await cot::tcp_accept(lfd);
        co_await server_one_deflate(std::move(cfd),
                                    cot::ws_connection_options::permessage_deflate);
        server_done.trigger();
    };
    server().detach();

    co_await cot::after(5ms);

    auto cfd = co_await cot::tcp_connect(addr);
    auto ws = cot::ws_parser::wrap_client(std::move(cfd), "localhost", "/",
                                          {}, cot::ws_connection_options::permessage_deflate);
    co_await ws.handshake();
    assert(ws.permessage_deflate_negotiated());

    // Build a 200 KB highly-compressible binary payload (repeating pattern).
    std::string payload;
    payload.resize(200 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = char((i / 7) & 0xff);
    }

    for (int round = 0; round < 3; ++round) {
        co_await ws.send_binary(payload);
        auto m = co_await ws.receive();
        auto& msg = std::get<cot::ws_message>(m);
        assert(msg.opcode == cot::ws_opcode::binary);
        assert(msg.payload == payload);
    }

    co_await ws.close();
    co_await cot::attempt(cot::task<>{server_done}, cot::after(2000ms));
    std::cerr << "deflate_multi_message: ok\n";
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

    run("text_echo", test_text_echo);
    run("binary_large", test_binary_large);
    run("server_close", test_server_close);
    run("bad_handshake", test_bad_handshake);
    run("deflate_round_trip", test_deflate_round_trip);
    run("deflate_client_off", test_deflate_client_off);
    run("deflate_server_off", test_deflate_server_off);
    run("deflate_multi_message", test_deflate_multi_message);

    if (ran == 0) {
        std::cerr << "No matching tests\n";
        return 1;
    }
    std::cerr << "*** done ***\n";
    return 0;
}
