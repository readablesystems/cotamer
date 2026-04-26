#include "cotamer/cotamer.hh"
#include "cotamer/curl.hh"
#include "cotamer/http.hh"
#include <curl/curl.h>
#include <cassert>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <string>

namespace cot = cotamer;
using namespace std::chrono_literals;

namespace {

uint16_t unique_port() {
    static uint16_t next = 29500;
    return next++;
}

// Minimal one-connection-at-a-time HTTP server. `handler` is called with the
// parsed request and returns the response to send. Server keeps running
// until `stop` is triggered.
cot::task<> run_server(uint16_t port,
                       std::function<cot::http_message(const cot::http_message&)> handler,
                       cot::event stop) {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:" + std::to_string(port));
    while (!stop.triggered()) {
        auto cfd = co_await cot::attempt(cot::tcp_accept(lfd), stop);
        if (!cfd) {
            break;
        }
        auto conn = [](cot::fd c,
                       std::function<cot::http_message(const cot::http_message&)> h)
                       -> cot::task<> {
            cot::http_parser hp(std::move(c), HTTP_REQUEST);
            while (true) {
                auto req = co_await hp.receive();
                if (!hp.ok()) {
                    break;
                }
                auto resp = h(req);
                co_await hp.send(std::move(resp));
                if (!hp.should_keep_alive()) {
                    break;
                }
            }
        };
        conn(std::move(*cfd), handler).detach();
    }
}

} // namespace


// TEST: basic GET, 200 OK, body round-tripped.
cot::task<> test_basic_get() {
    uint16_t port = unique_port();
    cot::event stop;
    auto handler = [](const cot::http_message& req) {
        cot::http_message res;
        std::string body = std::format("you requested {}", req.url());
        res.status_code(200)
           .header("Content-Type", "text/plain")
           .body(std::move(body));
        return res;
    };
    run_server(port, handler, stop).detach();
    co_await cot::after(5ms);

    auto url = std::format("http://127.0.0.1:{}/hello", port);
    auto resp = co_await cot::curl_fetch(url);
    assert(resp.status == 200);
    assert(resp.body == "you requested /hello");
    auto it = resp.headers.find("Content-Type");
    assert(it != resp.headers.end() && it->second == "text/plain");
    std::cerr << "basic_get: ok (status=" << resp.status
              << ", body=\"" << resp.body << "\")\n";

    stop.trigger();
    co_await cot::after(20ms);
}


// TEST: redirect is followed by default.
cot::task<> test_redirect() {
    uint16_t port = unique_port();
    cot::event stop;
    auto handler = [port](const cot::http_message& req) {
        cot::http_message res;
        if (req.url() == "/from") {
            res.status_code(302)
               .header("Location",
                       std::format("http://127.0.0.1:{}/to", port))
               .body("");
        } else {
            res.status_code(200)
               .header("Content-Type", "text/plain")
               .body("final");
        }
        return res;
    };
    run_server(port, handler, stop).detach();
    co_await cot::after(5ms);

    auto url = std::format("http://127.0.0.1:{}/from", port);
    auto resp = co_await cot::curl_fetch(url);
    assert(resp.status == 200);
    assert(resp.body == "final");
    std::cerr << "redirect: ok\n";

    stop.trigger();
    co_await cot::after(20ms);
    cot::curl_reset();
}


// Helper for test_stress; free function so the captured refs (in the
// caller's coroutine frame) stay valid for the lifetime of the coroutine.
cot::task<> stress_fetch(int i, uint16_t port,
                         int& outstanding, int& failures, cot::event done) {
    try {
        auto url = std::format("http://127.0.0.1:{}/req{}", port, i);
        auto resp = co_await cot::curl_fetch(url);
        if (resp.status != 200
            || resp.body != std::format("OK /req{}", i)) {
            ++failures;
        }
    } catch (...) {
        ++failures;
    }
    if (--outstanding == 0) {
        done.trigger();
    }
}

// TEST: many concurrent fetches against a single local server.
cot::task<> test_stress() {
    uint16_t port = unique_port();
    cot::event stop;
    auto handler = [](const cot::http_message& req) {
        cot::http_message res;
        res.status_code(200)
           .header("Content-Type", "text/plain")
           .body(std::format("OK {}", req.url()));
        return res;
    };
    run_server(port, handler, stop).detach();
    co_await cot::after(5ms);

    constexpr int N = 50;
    int outstanding = N;
    int failures = 0;
    cot::event all_done;

    for (int i = 0; i < N; ++i) {
        stress_fetch(i, port, outstanding, failures, all_done).detach();
    }
    co_await all_done;
    assert(failures == 0);
    std::cerr << "stress: ok (" << N << " concurrent fetches)\n";

    stop.trigger();
    co_await cot::after(50ms);
    cot::curl_reset();
}


// TORTURE: many rounds of many concurrent fetches. Designed to be run under
// `leaks --atExit -- build/cot-test-curl torture` to surface curl/cotamer
// memory leaks across thousands of transfers.
cot::task<> test_torture() {
    uint16_t port = unique_port();
    cot::event stop;
    auto handler = [](const cot::http_message& req) {
        cot::http_message res;
        res.status_code(200)
           .header("Content-Type", "text/plain")
           .body(std::format("OK {}", req.url()));
        return res;
    };
    run_server(port, handler, stop).detach();
    co_await cot::after(5ms);

    constexpr int ROUNDS = 10;
    constexpr int N = 100;
    int total_failures = 0;

    for (int round = 0; round < ROUNDS; ++round) {
        int outstanding = N;
        int failures = 0;
        cot::event all_done;
        for (int i = 0; i < N; ++i) {
            stress_fetch(round * N + i, port, outstanding, failures, all_done).detach();
        }
        co_await all_done;
        total_failures += failures;
    }

    assert(total_failures == 0);
    std::cerr << "torture: ok (" << ROUNDS << " rounds × "
              << N << " concurrent = " << (ROUNDS * N) << " fetches)\n";

    stop.trigger();
    co_await cot::after(50ms);
    cot::curl_reset();
}


// Free coroutine helpers used by test_cancel.

cot::task<> silent_conn(cot::fd c, cot::event stop) {
    // Hold the accepted connection open without ever writing a response, so
    // a curl_fetch against this server stays suspended in cr.done forever.
    co_await stop;
    (void) c;
}

cot::task<> silent_server(uint16_t port, cot::event stop) {
    auto lfd = co_await cot::tcp_listen("127.0.0.1:" + std::to_string(port));
    while (!stop.triggered()) {
        auto cfd = co_await cot::attempt(cot::tcp_accept(lfd), stop);
        if (!cfd) {
            break;
        }
        silent_conn(std::move(*cfd), stop).detach();
    }
}

// TEST: destroying the task<curl_response> while the fetch is suspended must
// tear down the curl handle cleanly (no UB, no leak), and the curl_driver
// must remain usable for subsequent fetches.
cot::task<> test_cancel() {
    uint16_t port = unique_port();
    cot::event stop;
    silent_server(port, stop).detach();
    co_await cot::after(5ms);

    auto url = std::format("http://127.0.0.1:{}/slow", port);
    // Give curl time to connect and send the request, but the server doesn't
    // respond in time
    auto optresp = co_await cot::attempt(cot::curl_fetch(url), cot::after(20ms));
    assert(!optresp);

    // Verify the driver is still healthy by issuing a fresh fetch.
    uint16_t port2 = unique_port();
    cot::event stop2;
    auto handler = [](const cot::http_message& req) {
        cot::http_message res;
        res.status_code(200)
           .header("Content-Type", "text/plain")
           .body(std::format("after-cancel {}", req.url()));
        return res;
    };
    run_server(port2, handler, stop2).detach();
    co_await cot::after(5ms);

    auto resp = co_await cot::curl_fetch(
        std::format("http://127.0.0.1:{}/check", port2));
    assert(resp.status == 200);
    assert(resp.body == "after-cancel /check");
    std::cerr << "cancel: ok\n";

    stop.trigger();
    stop2.trigger();
    co_await cot::after(50ms);
    cot::curl_reset();
}


// TEST: transport error on unreachable port throws.
cot::task<> test_error_path() {
    // Nothing listening on 1; connect should fail.
    bool threw = false;
    try {
        co_await cot::curl_fetch("http://127.0.0.1:1/");
    } catch (const cot::curl_error& e) {
        threw = true;
        std::cerr << "error_path: caught code=" << e.code()
                  << " (" << e.what() << ")\n";
    }
    assert(threw);
}


cot::task<> run_all(int argc, char* argv[]) {
    int& ran = *new int(0);  // leak, we just care about count for error reporting
    auto should_run = [&](const char* name) {
        if (argc == 1) return true;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(name, argv[i]) == 0) return true;
        }
        return false;
    };
    auto run = [&](const char* name, auto fn) -> cot::task<> {
        if (!should_run(name)) co_return;
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        co_await fn();
    };

    co_await run("basic_get",   test_basic_get);
    co_await run("redirect",    test_redirect);
    co_await run("stress",      test_stress);
    co_await run("torture",     test_torture);
    co_await run("cancel",      test_cancel);
    co_await run("error_path",  test_error_path);

    if (ran == 0) {
        std::cerr << "No matching tests\n";
        std::exit(1);
    }
    std::cerr << "*** done ***\n";
    std::exit(0);
}

int main(int argc, char* argv[]) {
    cot::set_clock(cot::clock::real_time);
    run_all(argc, argv).detach();
    cot::loop();
    return 0;
}
