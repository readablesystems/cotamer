// examples/jsond-tester.cc
//    End-to-end test of `jsond` using Cotamer's native HTTP client
//    (`cot::tcp_connect` + `cot::http_parser`). Connects to a separately-
//    started jsond and drives it through the same scenarios as the curl
//    smoke test. Exit status is the number of failed checks.
//
//    Usage:
//        ./jsond -p 21997 &
//        ./jsond-tester -p 21997

#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <unistd.h>

namespace cot = cotamer;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

int g_ran = 0;
int g_failed = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_ran;                                                     \
        if (!(cond)) {                                               \
            ++g_failed;                                              \
            std::cerr << "  FAIL " << __func__ << ":" << __LINE__    \
                      << "  " #cond "\n";                            \
        }                                                            \
    } while (0)


struct response {
    unsigned status = 0;
    std::string body;
};

struct request_opts {
    llhttp_method method = HTTP_GET;
    std::string url;
    std::string body;
    std::string content_type;
};

// One request per TCP connection. jsond supports keep-alive but a fresh
// connection per call keeps test logic simple.
cot::task<response> http_call(std::string addr, request_opts r) {
    auto cfd = co_await cot::tcp_connect(addr);
    cot::http_parser hp(std::move(cfd), HTTP_RESPONSE);
    cot::http_message m;
    m.method(r.method)
        .url(std::move(r.url))
        .header("Host", "localhost")
        .header("Connection", "close");
    if (!r.body.empty()) {
        if (!r.content_type.empty()) {
            m.header("Content-Type", std::move(r.content_type));
        }
        m.body(std::move(r.body));
    }
    co_await hp.send(std::move(m));
    auto resp = co_await hp.receive();
    if (!hp.ok()) {
        throw std::runtime_error(
            std::format("http parse error: {}", llhttp_errno_name(hp.error())));
    }
    co_return response{resp.status_code(), resp.body()};
}


cot::task<> run_tests(std::string addr) {
    std::cerr << "create #1\n";
    auto r1 = co_await http_call(addr, {HTTP_POST, "/notes",
        R"({"title":"hello","body":"world"})", "application/json"});
    CHECK(r1.status == 201);
    auto j1 = json::parse(r1.body);
    CHECK(j1.value("title", "") == "hello");
    CHECK(j1.value("body", "") == "world");
    CHECK(j1["id"].is_number_unsigned());
    uint64_t id1 = j1["id"];

    std::cerr << "create #2\n";
    auto r2 = co_await http_call(addr, {HTTP_POST, "/notes",
        R"({"title":"second","body":"note"})", "application/json"});
    CHECK(r2.status == 201);
    uint64_t id2 = json::parse(r2.body)["id"];
    CHECK(id2 == id1 + 1);

    std::cerr << "list\n";
    auto r3 = co_await http_call(addr, {HTTP_GET, "/notes", "", ""});
    CHECK(r3.status == 200);
    auto j3 = json::parse(r3.body);
    CHECK(j3["notes"].is_array());
    CHECK(j3["notes"].size() == 2);

    std::cerr << "get one\n";
    auto r4 = co_await http_call(addr,
        {HTTP_GET, std::format("/notes/{}", id1), "", ""});
    CHECK(r4.status == 200);
    CHECK(json::parse(r4.body).value("title", "") == "hello");

    std::cerr << "put (partial update)\n";
    auto r5 = co_await http_call(addr,
        {HTTP_PUT, std::format("/notes/{}", id1),
         R"({"title":"renamed"})", "application/json"});
    CHECK(r5.status == 200);
    auto j5 = json::parse(r5.body);
    CHECK(j5.value("title", "") == "renamed");
    CHECK(j5.value("body", "") == "world");

    std::cerr << "stats\n";
    auto r6 = co_await http_call(addr, {HTTP_GET, "/stats", "", ""});
    CHECK(r6.status == 200);
    auto j6 = json::parse(r6.body);
    CHECK(j6["count"] == 2);
    CHECK(j6["recent_ids"].is_array());
    CHECK(j6["server"]["name"] == "jsond");

    std::cerr << "delete\n";
    auto r7 = co_await http_call(addr,
        {HTTP_DELETE, std::format("/notes/{}", id2), "", ""});
    CHECK(r7.status == 200);
    CHECK(json::parse(r7.body)["deleted"] == id2);

    std::cerr << "stats after delete\n";
    auto r8 = co_await http_call(addr, {HTTP_GET, "/stats", "", ""});
    CHECK(r8.status == 200);
    CHECK(json::parse(r8.body)["count"] == 1);

    std::cerr << "404 unknown id\n";
    auto r9 = co_await http_call(addr,
        {HTTP_GET, "/notes/9999", "", ""});
    CHECK(r9.status == 404);

    std::cerr << "404 unknown route\n";
    auto r10 = co_await http_call(addr, {HTTP_GET, "/unknown", "", ""});
    CHECK(r10.status == 404);

    std::cerr << "405 method not allowed\n";
    auto r11 = co_await http_call(addr,
        {HTTP_PATCH, "/notes", "", ""});
    CHECK(r11.status == 405);

    std::cerr << "400 invalid JSON\n";
    auto r12 = co_await http_call(addr,
        {HTTP_POST, "/notes", "{not-json", "application/json"});
    CHECK(r12.status == 400);

    std::cerr << "400 missing field\n";
    auto r13 = co_await http_call(addr,
        {HTTP_POST, "/notes", R"({"title":"x"})", "application/json"});
    CHECK(r13.status == 400);

    std::cerr << "delete missing id\n";
    auto r14 = co_await http_call(addr,
        {HTTP_DELETE, "/notes/9999", "", ""});
    CHECK(r14.status == 404);
}


cot::task<> main_task(std::string addr) {
    try {
        co_await run_tests(addr);
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        ++g_failed;
    }
    std::cerr << (g_failed == 0 ? "PASS " : "FAIL ")
              << g_ran << " checks, " << g_failed << " failures\n";
    std::exit(g_failed == 0 ? 0 : 1);
}

void usage() {
    std::cerr << "Usage: jsond-tester [-h HOST] [-p PORT]\n"
              << "  -h HOST  jsond hostname (default localhost)\n"
              << "  -p PORT  jsond port (default 11112, matches jsond)\n";
}

} // namespace


int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 11112;

    int opt;
    while ((opt = getopt(argc, argv, "?h:p:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = std::strtol(optarg, nullptr, 0); break;
        case '?':
        default: usage(); return 1;
        }
    }
    if (optind != argc) {
        usage();
        return 1;
    }

    auto addr = std::format("{}:{}", host, port);
    cot::set_clock(cot::clock::real_time);
    cot::task<> t = main_task(addr);
    cot::loop();
    return g_failed == 0 ? 0 : 1;
}
