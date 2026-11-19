#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace cot = cotamer;
using namespace std::chrono_literals;

namespace {

void make_socketpair(cot::fd& a, cot::fd& b) {
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(r == 0);
    (void) r;
    a = cot::fd(sv[0]);
    b = cot::fd(sv[1]);
    cot::set_nonblocking(a);
    cot::set_nonblocking(b);
}

cot::task<std::string> read_all(cot::fd f) {
    std::string out;
    char buf[4096];
    while (true) {
        auto r = co_await cot::recv(f, buf, sizeof(buf));
        if (!r || *r == 0) {
            co_return out;
        }
        out.append(buf, *r);
    }
}

// Return the bytes `http_parser` sends for `m` (a response, or a request if
// `request` is true).
cot::task<std::string> serialize(cot::http_message m, bool request = false) {
    cot::fd a, b;
    make_socketpair(a, b);
    {
        cot::http_parser hp(std::move(a), request ? cot::http_parser::client
                                                  : cot::http_parser::server);
        if (request) {
            (void) co_await hp.send_request(std::move(m));
        } else {
            co_await hp.send_response(std::move(m));
        }
    }
    co_return co_await read_all(std::move(b));
}

template <typename F>
bool throws_invalid_argument(F f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

template <typename T>
cot::task<bool> task_throws_invalid_argument(cot::task<T> t) {
    try {
        co_await std::move(t);
    } catch (const std::invalid_argument&) {
        co_return true;
    }
    co_return false;
}

size_t count(std::string_view haystack, std::string_view needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + 1)) {
        ++n;
    }
    return n;
}

} // namespace


// TEST: Status codes absent from the status table are sent with an empty
// reason phrase (llhttp_status_name() aborts on unknown codes).
cot::task<> test_unknown_status_code() {
    for (unsigned code : {270U, 599U, 600U, 999U}) {
        auto out = co_await serialize(cot::http_message(code));
        assert(out.starts_with(std::format("HTTP/1.1 {} \r\n", code)));
    }
    auto out = co_await serialize(cot::http_message(110));
    assert(out.starts_with("HTTP/1.1 110 \r\n"));
    out = co_await serialize(cot::http_message(404));
    assert(out.starts_with("HTTP/1.1 404 Not Found\r\n"));
}


// TEST: CR, LF, and NUL are rejected in header values; header names must be
// tokens.
cot::task<> test_header_injection() {
    cot::http_message m(302);
    assert(throws_invalid_argument([&] { m.header("Location", "/x\r\nSet-Cookie: a=1"); }));
    assert(throws_invalid_argument([&] { m.header("Location", "/x\nSet-Cookie: a=1"); }));
    assert(throws_invalid_argument([&] { m.header("Location", "/x\rSet-Cookie: a=1"); }));
    assert(throws_invalid_argument([&] { m.header("Location", std::string_view("/x\0y", 4)); }));
    assert(throws_invalid_argument([&] { m.header("X-A\r\nX-B", "1"); }));
    assert(throws_invalid_argument([&] { m.header("X-A: 1", "1"); }));
    assert(throws_invalid_argument([&] { m.header("X A", "1"); }));
    assert(throws_invalid_argument([&] { m.header("", "1"); }));
    assert(!m.has_header("location"));

    // Ordinary header values with spaces, tabs, semicolons, and non-ASCII
    // bytes are fine
    m.header("Content-Type", "text/html; charset=utf-8")
        .header("X-Spaces", "a\tb c")
        .header("X-Utf8", "caf\xc3\xa9");
    assert(m.header("content-type") == "text/html; charset=utf-8");

    // Status message
    cot::http_message bad_status;
    bad_status.status_code(200, "OK\r\nSet-Cookie: a=1");
    assert(co_await task_throws_invalid_argument(serialize(std::move(bad_status))));

    // Request target
    for (std::string url : {std::string("/x\r\nHost: evil"),
                            std::string("/x HTTP/1.1"),
                            std::string("/x\0y", 4)}) {
        cot::http_message req(HTTP_GET, url);
        assert(co_await task_throws_invalid_argument(serialize(std::move(req), true)));
    }

    // Host set on the parser
    {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser hp(std::move(a), cot::http_parser::client, "x\r\nX-Evil: 1");
        assert(co_await task_throws_invalid_argument(hp.send_request(cot::http_message(HTTP_GET, "/"))));
    }
}


// TEST: A copied message keeps its headers visible to has_header(), so
// sending does not add a second Content-Length.
cot::task<> test_copy_keeps_headers() {
    cot::http_message m(200);
    m.header("Content-Length", "5").body("hello");

    cot::http_message c(m);
    assert(c.has_header("content-length"));
    assert(c.header("content-length") == "5");
    auto out = co_await serialize(std::move(c));
    assert(count(out, "Content-Length") == 1);

    cot::http_message assigned;
    assigned = m;
    assert(assigned.has_header("content-length"));
    out = co_await serialize(std::move(assigned));
    assert(count(out, "Content-Length") == 1);
}


// TEST: clear() removes headers.
cot::task<> test_clear_removes_headers() {
    cot::http_message m(200);
    m.header("Set-Cookie", "session=alice").body("for alice");
    m.clear();
    assert(!m.has_header("set-cookie"));
    assert(m.header_begin() == m.header_end());
    m.status_code(200).body("for bob");
    auto out = co_await serialize(std::move(m));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nfor bob");
}


// TEST: An empty chunk does not terminate a chunked response.
cot::task<> test_empty_chunk() {
    cot::fd a, b;
    make_socketpair(a, b);
    auto server = [](cot::fd f) -> cot::task<> {
        cot::http_parser hp(std::move(f), cot::http_parser::server);
        cot::http_message res(200);
        res.header("Transfer-Encoding", "chunked");
        co_await hp.send_response(std::move(res));
        co_await hp.send_response_chunk("abc");
        co_await hp.send_response_chunk("");
        co_await hp.send_response_chunk("def");
        co_await hp.send_response_end_chunk();
        co_await hp.send_response(cot::http_message(204));
    }(std::move(a));

    cot::http_parser client(std::move(b), cot::http_parser::client);
    auto res = co_await client.receive();
    assert(client.ok());
    assert(res.status_code() == 200);
    assert(res.body() == "abcdef");
    res = co_await client.receive();
    assert(client.ok());
    assert(res.status_code() == 204);
    co_await std::move(server);
}


// TEST: Status codes outside 100–999 are rejected.
cot::task<> test_invalid_status_code() {
    for (unsigned code : {0U, 99U, 1000U, 70000U}) {
        assert(co_await task_throws_invalid_argument(serialize(cot::http_message(code))));
    }
}


namespace {

// Write `wire` to a server-mode parser (configured by `config`) and return
// the parser's failure after one receive(). The writing end stays open.
template <typename Config>
cot::task<cot::http_parser::failure_type> receive_failure(std::string wire, Config config) {
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::server);
    hp.set_receive_timeout(2s);
    config(hp);
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;
    auto req = co_await hp.receive();
    assert(!hp.ok() || hp.failure() == cot::http_parser::failure_type::none);
    if (!hp.ok()) {
        assert(!req.ok());
    }
    co_return hp.failure();
}

auto no_config = [](cot::http_parser&) {};

} // namespace


// TEST: Header and body size limits.
cot::task<> test_size_limits() {
    using ft = cot::http_parser::failure_type;
    std::string longheader = "GET / HTTP/1.1\r\nX-Long: " + std::string(2000, 'x') + "\r\n\r\n";
    assert(co_await receive_failure(longheader, [](auto& hp) { hp.set_max_header_size(1024); })
           == ft::header_too_large);
    assert(co_await receive_failure(longheader, no_config) == ft::none);

    std::string longurl = "GET /" + std::string(2000, 'x') + " HTTP/1.1\r\n\r\n";
    assert(co_await receive_failure(longurl, [](auto& hp) { hp.set_max_header_size(1024); })
           == ft::header_too_large);

    // A too-large Content-Length is rejected before the body arrives
    std::string cl = "POST / HTTP/1.1\r\nContent-Length: 1000\r\n\r\n";
    assert(co_await receive_failure(cl, [](auto& hp) { hp.set_max_body_size(100); })
           == ft::body_too_large);
    assert(co_await receive_failure(cl + std::string(1000, 'x'), no_config) == ft::none);

    // Enormous lengths fail cleanly
    assert(co_await receive_failure("POST / HTTP/1.1\r\nContent-Length: 9000000000000000000\r\n\r\nab",
                                    no_config)
           == ft::body_too_large);
    assert(co_await receive_failure("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n7FFFFFFFFFFFFFF\r\nab",
                                    no_config)
           == ft::body_too_large);

    // Chunks accumulate toward the limit
    std::string chunked = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n5\r\nhello\r\n5\r\nhello\r\n0\r\n\r\n";
    assert(co_await receive_failure(chunked, [](auto& hp) { hp.set_max_body_size(12); })
           == ft::body_too_large);
    assert(co_await receive_failure(chunked, [](auto& hp) { hp.set_max_body_size(15); })
           == ft::none);
}


// TEST: Receive timeouts.
cot::task<> test_receive_timeout() {
    using ft = cot::http_parser::failure_type;
    {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser server(a, cot::http_parser::server);
        assert(server.receive_timeout() == cot::http_parser::default_server_receive_timeout);
        cot::http_parser client(b, cot::http_parser::client);
        assert(client.receive_timeout() == cot::duration::max());
    }

    // A partial request times out
    assert(co_await receive_failure("GET / HTTP/1.1\r\nHost:",
                                    [](auto& hp) { hp.set_receive_timeout(50ms); })
           == ft::timeout);
    // So does an idle connection
    assert(co_await receive_failure("", [](auto& hp) { hp.set_receive_timeout(50ms); })
           == ft::timeout);

    // After a timeout, receive() fails immediately
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::server);
    hp.set_receive_timeout(50ms);
    (void) co_await hp.receive();
    assert(hp.failure() == ft::timeout);
    assert(std::string_view(hp.error_reason()) == "receive timeout");
    auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && !r->ok());
}


// TEST: Cancelling receive() mid-message fails the parser; cancelling it
// between messages does not.
cot::task<> test_receive_cancel() {
    using ft = cot::http_parser::failure_type;
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::server);

    // Cancel between messages: parser remains usable
    auto r = co_await cot::attempt(hp.receive(), cot::after(20ms));
    assert(!r);
    assert(hp.ok());
    std::string req = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t nw = ::write(b.fileno(), req.data(), req.size());
    assert(nw == ssize_t(req.size()));
    auto msg = co_await hp.receive();
    assert(hp.ok() && msg.url() == "/a");

    // Cancel mid-message: parser fails
    std::string partial = "GET /b HTTP/1.1\r\nHo";
    nw = ::write(b.fileno(), partial.data(), partial.size());
    assert(nw == ssize_t(partial.size()));
    (void) nw;
    r = co_await cot::attempt(hp.receive(), cot::after(20ms));
    assert(!r);
    assert(!hp.ok());
    assert(hp.failure() == ft::cancelled);
    assert(!hp.should_keep_alive());
    r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && !r->ok());
}


// TEST: After an upgrade request, the parser stops parsing HTTP.
cot::task<> test_upgrade() {
    for (std::string head : {std::string("GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"),
                             std::string("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n")}) {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser hp(std::move(a), cot::http_parser::server);
        std::string wire = head + "GET /admin HTTP/1.1\r\nHost: x\r\n\r\n";
        ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
        assert(nw == ssize_t(wire.size()));
        (void) nw;

        auto req = co_await hp.receive();
        assert(hp.ok() && req.ok());
        assert(req.upgrade());
        assert(!hp.should_keep_alive());
        assert(hp.receive_buffer() == "GET /admin HTTP/1.1\r\nHost: x\r\n\r\n");

        auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
        assert(r && !r->ok());
        assert(hp.error() == HPE_PAUSED_UPGRADE);
    }

    // Ordinary requests are not upgrades
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::server);
    std::string wire = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;
    auto req = co_await hp.receive();
    assert(hp.ok() && !req.upgrade() && hp.should_keep_alive());
}


// TEST: Response framing: Content-Length defaults, bodyless statuses, HEAD.
cot::task<> test_response_framing() {
    auto out = co_await serialize(cot::http_message(404));
    assert(out == "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");

    for (unsigned code : {204U, 304U, 100U}) {
        cot::http_message m(code);
        m.body("ignored");
        out = co_await serialize(std::move(m));
        assert(out.ends_with("\r\n\r\n"));
        assert(out.find("Content-Length") == std::string::npos);
    }

    // Explicit framing headers are respected
    cot::http_message chunked(200);
    chunked.header("Transfer-Encoding", "chunked");
    out = co_await serialize(std::move(chunked));
    assert(out.find("Content-Length") == std::string::npos);

    // Pipelined GET then HEAD: the HEAD response omits the body
    cot::fd a, b;
    make_socketpair(a, b);
    {
        cot::http_parser hp(std::move(a), cot::http_parser::server);
        std::string wire = "GET /x HTTP/1.1\r\nHost: x\r\n\r\n"
            "HEAD /x HTTP/1.1\r\nHost: x\r\n\r\n"
            "HEAD /y HTTP/1.1\r\nHost: x\r\n\r\n";
        ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
        assert(nw == ssize_t(wire.size()));
        (void) nw;
        auto r1 = co_await hp.receive();
        auto r2 = co_await hp.receive();
        auto r3 = co_await hp.receive();
        assert(r1.method() == HTTP_GET && r2.method() == HTTP_HEAD && r3.method() == HTTP_HEAD);
        co_await hp.send(cot::http_message(200).body("hello"));
        co_await hp.send(cot::http_message(200).body("hello"));
        co_await hp.send(cot::http_message(200));
    }
    out = co_await read_all(std::move(b));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"
                  "HTTP/1.1 200 OK\r\n\r\n");
}


// TEST: A client parser knows that responses to HEAD have no body.
cot::task<> test_client_head() {
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::client, "x");
    auto t1 = co_await hp.send_request(cot::http_message(HTTP_HEAD, "/x"));
    auto t2 = co_await hp.send_request(cot::http_message(HTTP_HEAD, "/huge"));
    auto t3 = co_await hp.send_request(cot::http_message(HTTP_GET, "/x"));
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 100000000000\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;

    auto r1 = co_await cot::attempt(hp.receive(std::move(t1)), cot::after(1s));
    assert(r1 && r1->ok());
    assert(r1->body().empty() && r1->header("content-length") == "5");
    // the body limit does not apply to HEAD responses
    auto r2 = co_await cot::attempt(hp.receive(std::move(t2)), cot::after(1s));
    assert(r2 && r2->ok());
    assert(r2->body().empty());
    auto r3 = co_await cot::attempt(hp.receive(std::move(t3)), cot::after(1s));
    assert(r3 && r3->ok());
    assert(r3->body() == "hello");
    assert(hp.should_keep_alive());
}


// TEST: A client parser treats a 2xx response to CONNECT as an upgrade.
cot::task<> test_client_connect() {
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::client);
    auto t = co_await hp.send_request(cot::http_message(HTTP_CONNECT, "example.com:443"));
    std::string wire = "HTTP/1.1 200 Connection Established\r\n\r\nTUNNEL";
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;

    auto r = co_await cot::attempt(hp.receive(std::move(t)), cot::after(1s));
    assert(r && r->ok() && hp.ok());
    assert(r->status_code() == 200 && r->upgrade());
    assert(!hp.should_keep_alive());
    assert(hp.receive_buffer() == "TUNNEL");
}


// TEST: decoded_path() decodes and normalizes paths.
cot::task<> test_decoded_path() {
    auto dp = [](std::string url) {
        return cot::http_message(HTTP_GET, std::move(url)).decoded_path();
    };
    assert(dp("/") == "/");
    assert(dp("/a/b?x=1#h") == "/a/b");
    assert(dp("/a/b/") == "/a/b/");
    assert(dp("/a%20b/c") == "/a b/c");
    assert(dp("/a/./b") == "/a/b");
    assert(dp("/a/.") == "/a/");
    assert(dp("/a/b/../c") == "/a/c");
    assert(dp("/a/..") == "/");
    assert(dp("/a//../b") == "/a/b");
    assert(dp("/%2e%2E/x") == std::nullopt);
    assert(dp("/a/%2e%2e") == "/");
    assert(dp("/..") == std::nullopt);
    assert(dp("/a/../..") == std::nullopt);
    assert(dp("/a%2fb") == std::nullopt);
    assert(dp("/a%00b") == std::nullopt);
    assert(dp("/a%2") == std::nullopt);
    assert(dp("/a%zz") == std::nullopt);
    assert(dp("http://example.com/") == std::nullopt);
    assert(dp("") == std::nullopt);
    co_return;
}


int main(int argc, char* argv[]) {
    unsigned ran = 0;
    cot::set_clock(cot::clock::real_time);

    auto run = [&](const char* name, auto fn) {
        bool found = argc == 1;
        for (int argi = 1; !found && argi < argc; ++argi) {
            found = strcmp(name, argv[argi]) == 0;
        }
        if (!found) {
            return;
        }
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        cot::reset();
        auto t = fn();
        cot::loop();
        assert(t.done() && "test did not complete");
    };

    run("unknown_status_code", test_unknown_status_code);
    run("header_injection", test_header_injection);
    run("copy_keeps_headers", test_copy_keeps_headers);
    run("clear_removes_headers", test_clear_removes_headers);
    run("empty_chunk", test_empty_chunk);
    run("invalid_status_code", test_invalid_status_code);
    run("size_limits", test_size_limits);
    run("receive_timeout", test_receive_timeout);
    run("receive_cancel", test_receive_cancel);
    run("upgrade", test_upgrade);
    run("response_framing", test_response_framing);
    run("client_head", test_client_head);
    run("client_connect", test_client_connect);
    run("decoded_path", test_decoded_path);

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
