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
        auto ac = cot::tcp_accept(lfd);
        co_await cot::any(ac.resolution(), stop);
        if (stop.triggered()) {
            co_return;
        }
        auto cfd = co_await ac;
        auto conn = [](cot::fd c,
                       std::function<cot::http_message(const cot::http_message&)> h)
                       -> cot::task<> {
            cot::http_parser hp(std::move(c), HTTP_REQUEST);
            while (true) {
                auto req = co_await hp.receive();
                if (!hp.ok()) break;
                auto resp = h(req);
                co_await hp.send(std::move(resp));
                if (!hp.should_keep_alive()) break;
            }
        };
        conn(std::move(cfd), handler).detach();
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


// TEST: transport error on unreachable port throws.
cot::task<> test_error_path() {
    // Nothing listening on 1; connect should fail.
    bool threw = false;
    try {
        co_await cot::curl_fetch("http://127.0.0.1:1/");
    } catch (const std::system_error& e) {
        threw = true;
        std::cerr << "error_path: caught (" << e.what() << ")\n";
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
