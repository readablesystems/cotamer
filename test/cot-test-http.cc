#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "cotamer/http_fields.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#include <print>
#include <ranges>
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

// Parse one message from `wire`, which the peer sends and then closes.
cot::task<cot::http_message> parse(std::string wire, bool response = false) {
    cot::fd a, b;
    make_socketpair(a, b);
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;
    b.close();
    cot::http_parser hp(std::move(a), response ? cot::http_parser::client
                                               : cot::http_parser::server);
    auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && r->ok());
    co_return std::move(*r);
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
    // a response reports its request's method
    assert(r1->method() == HTTP_HEAD);
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


// TEST: Dropping a ticket gives up its place in line.
cot::task<> test_dropped_ticket() {
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::client, "x");
    std::string wire;
    for (auto body : {"a", "b", "c", "d"}) {
        wire += std::format("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n{}", body);
    }
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;

    // `send()` drops the ticket
    co_await hp.send(cot::http_message(HTTP_GET, "/a"));
    auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && r->ok() && r->body() == "a");

    // A receive() without a ticket waits behind tickets still held
    auto tb = co_await hp.send_request(cot::http_message(HTTP_GET, "/b"));
    (void) co_await hp.send_request(cot::http_message(HTTP_GET, "/c"));
    auto rc = hp.receive();
    auto td = co_await hp.send_request(cot::http_message(HTTP_GET, "/d"));
    r = co_await cot::attempt(hp.receive(std::move(tb)), cot::after(1s));
    assert(r && r->ok() && r->body() == "b");
    r = co_await cot::attempt(std::move(rc), cot::after(1s));
    assert(r && r->ok() && r->body() == "c");
    r = co_await cot::attempt(hp.receive(std::move(td)), cot::after(1s));
    assert(r && r->ok() && r->body() == "d");
}


// TEST: A response without Content-Length or chunking ends at EOF.
cot::task<> test_close_delimited() {
    {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser hp(std::move(a), cot::http_parser::client);
        std::string wire = "HTTP/1.1 200 OK\r\n\r\nhello";
        ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
        assert(nw == ssize_t(wire.size()));
        (void) nw;
        b.close();
        auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
        assert(r && r->ok() && hp.ok());
        assert(r->status_code() == 200 && r->body() == "hello");
        assert(!hp.should_keep_alive());
        r = co_await cot::attempt(hp.receive(), cot::after(1s));
        assert(r && !r->ok() && hp.error() == HPE_CLOSED_CONNECTION);
    }

    // A truncated body is an error
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::client);
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;
    b.close();
    auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && !r->ok() && hp.error() == HPE_CLOSED_CONNECTION);
}


// TEST: After a parse error, receive() keeps failing with the same error.
cot::task<> test_receive_after_error() {
    cot::fd a, b;
    make_socketpair(a, b);
    cot::http_parser hp(std::move(a), cot::http_parser::server);
    std::string wire = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "BOGUS\x01 / HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /c HTTP/1.1\r\nHost: x\r\n\r\n";
    ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
    assert(nw == ssize_t(wire.size()));
    (void) nw;
    auto r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && r->ok() && r->url() == "/a");
    r = co_await cot::attempt(hp.receive(), cot::after(1s));
    assert(r && !r->ok() && !hp.ok());
    auto err = hp.error();
    std::string rest = hp.receive_buffer();
    for (int i = 0; i != 3; ++i) {
        r = co_await cot::attempt(hp.receive(), cot::after(1s));
        assert(r && !r->ok() && r->error() == err && hp.error() == err);
        assert(hp.receive_buffer() == rest);
    }
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
    assert(dp("http://example.com/a/../b") == "/b");
    assert(dp("http://example.com") == "/");
    assert(dp("*") == std::nullopt);
    assert(dp("") == std::nullopt);
    co_return;
}


// TEST: remove_header removes every header with a given name, and
// remove_header_if removes headers that match a predicate.
cot::task<> test_remove_header() {
    cot::http_message m(200);
    m.header("A", "1").header("B", "2").header("a", "3").header("C", "4");
    m.remove_header("x");
    assert(m.header_end() - m.header_begin() == 4);
    m.remove_header("A");
    assert(!m.has_header("a"));
    assert(m.header("b") == "2" && m.header("c") == "4");
    auto it = m.header_begin();
    assert(it.name() == "B" && it.value() == "2");
    ++it;
    assert(it.name() == "C" && it.value() == "4");
    ++it;
    assert(it == m.header_end());
    m.header("D", "5");
    auto out = co_await serialize(std::move(m));
    assert(out == "HTTP/1.1 200 OK\r\nB: 2\r\nC: 4\r\nD: 5\r\n"
                  "Content-Length: 0\r\n\r\n");

    // received headers, including trailing whitespace
    auto r = co_await parse("GET / HTTP/1.1\r\nHost: x\r\nX-A: 1  \r\n"
                            "X-B:2 \r\nX-A: 3\r\nX-C: 4\r\n\r\n");
    r.remove_header("host").remove_header("X-A");
    assert(r.header_end() - r.header_begin() == 2);
    assert(r.header("x-b") == "2" && r.header("x-c") == "4");
    out = co_await serialize(std::move(r), true);
    assert(out == "GET / HTTP/1.1\r\nX-B: 2\r\nX-C: 4\r\n\r\n");

    cot::http_message all(200);
    all.header("A", "1").header("A", "2").remove_header("a");
    assert(all.header_begin() == all.header_end());
    out = co_await serialize(std::move(all));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    // remove_header_if; copies of a message with removed headers
    cot::http_message p(200);
    p.add_header("Keep", "1").add_header("Drop", "x").add_header("Keep", "2")
        .add_header("Also", "drop");
    p.remove_header_if([](std::string_view name, std::string_view value) {
        return name == "Drop" || value == "drop";
    });
    assert(p.header("keep") == "1, 2" && !p.has_header("drop"));
    cot::http_message pc(p);
    assert(pc.header("keep") == "1, 2" && !pc.has_header("drop"));
    out = co_await serialize(std::move(pc));
    assert(out == "HTTP/1.1 200 OK\r\nKeep: 1\r\nKeep: 2\r\n"
                  "Content-Length: 0\r\n\r\n");

    // several holes
    cot::http_message h(200);
    h.header("X", "0");
    for (int i = 1; i <= 7; ++i) {
        h.header(std::format("H{}", i), std::to_string(i));
        if (i == 3 || i == 6) {
            h.header("X", "x").header("X", "y");
        }
    }
    h.remove_header("x");
    int i = 1;
    for (auto it = h.header_begin(); it != h.header_end(); ++it, ++i) {
        assert(it.name() == std::format("H{}", i) && it.value() == std::to_string(i));
    }
    assert(i == 8);
    out = co_await serialize(std::move(h));
    assert(out == "HTTP/1.1 200 OK\r\nH1: 1\r\nH2: 2\r\nH3: 3\r\nH4: 4\r\n"
                  "H5: 5\r\nH6: 6\r\nH7: 7\r\nContent-Length: 0\r\n\r\n");

    // a throwing predicate keeps the removals made so far
    cot::http_message t(200);
    t.header("A", "1").header("B", "2").header("C", "3").header("D", "4");
    try {
        t.remove_header_if([](std::string_view name, std::string_view) {
            if (name == "C") {
                throw std::runtime_error("x");
            }
            return name == "A";
        });
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(t.header_end() - t.header_begin() == 3);
    out = co_await serialize(std::move(t));
    assert(out == "HTTP/1.1 200 OK\r\nB: 2\r\nC: 3\r\nD: 4\r\n"
                  "Content-Length: 0\r\n\r\n");
}


// TEST: A message with a body is sent with a Content-Length that matches the
// body, replacing other framing headers, so received messages can be
// forwarded.
cot::task<> test_forward_framing() {
    // A received chunked request keeps its headers, but is forwarded with
    // Content-Length
    auto r = co_await parse("POST /x HTTP/1.1\r\nHost: x\r\n"
                            "Transfer-Encoding: chunked\r\nX-Y: 1\r\n\r\n"
                            "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n");
    assert(r.body() == "hello" && r.header("transfer-encoding") == "chunked");
    auto out = co_await serialize(std::move(r), true);
    assert(out == "POST /x HTTP/1.1\r\nHost: x\r\nX-Y: 1\r\n"
                  "Content-Length: 5\r\n\r\nhello");
    r = co_await parse(out);
    assert(r.body() == "hello");

    // An empty chunked body is forwarded as Content-Length: 0
    r = co_await parse("POST /x HTTP/1.1\r\nHost: x\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    out = co_await serialize(std::move(r), true);
    assert(out == "POST /x HTTP/1.1\r\nHost: x\r\n"
                  "Content-Length: 0\r\n\r\n");

    // A request without a body is forwarded unchanged
    r = co_await parse("GET /x HTTP/1.1\r\nHost: x\r\n\r\n");
    out = co_await serialize(std::move(r), true);
    assert(out == "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");

    // Received chunked response
    r = co_await parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "3\r\nabc\r\n0\r\n\r\n", true);
    out = co_await serialize(std::move(r));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");

    // A changed body gets a new Content-Length
    r = co_await parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", true);
    r.body("hello world");
    out = co_await serialize(std::move(r));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world");

    // A user's mismatched framing headers are replaced
    cot::http_message m(200);
    m.header("Content-Length", "10").body("hi");
    out = co_await serialize(std::move(m));
    assert(out == "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    cot::http_message req(HTTP_POST, "/");
    req.header("Transfer-Encoding", "chunked").header("Content-Length", "10").body("hi");
    out = co_await serialize(std::move(req), true);
    assert(out == "POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi");

    // A received response to HEAD has no body; its Content-Length is kept
    {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser hp(std::move(a), cot::http_parser::client, "x");
        auto t = co_await hp.send_request(cot::http_message(HTTP_HEAD, "/x"));
        std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        ssize_t nw = ::write(b.fileno(), wire.data(), wire.size());
        assert(nw == ssize_t(wire.size()));
        (void) nw;
        auto rr = co_await cot::attempt(hp.receive(std::move(t)), cot::after(1s));
        assert(rr && rr->ok());
        out = co_await serialize(std::move(*rr));
        assert(out == wire);
    }
}


// Write `wire` to `f`, either all at once or a byte at a time with pauses.
cot::task<> feed(cot::fd f, std::string wire, bool slowly) {
    if (!slowly) {
        co_await cot::write(f, wire.data(), wire.size());
        co_return;
    }
    for (char c : wire) {
        co_await cot::write(f, &c, 1);
        co_await cot::after(1ms);
    }
}

// TEST: Pipelined requests, with and without bodies, are received as
// separate messages, whether they arrive together or a byte at a time.
cot::task<> test_pipelining() {
    std::string wire = "GET /first HTTP/1.1\r\nHost: x\r\n\r\n"
        "POST /p1 HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
        "POST /p2 HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nwor\r\n4\r\nld!!\r\n0\r\n\r\n"
        "GET /last HTTP/1.1\r\nHost: x\r\n\r\n";
    for (bool slowly : {false, true}) {
        cot::fd a, b;
        make_socketpair(a, b);
        cot::http_parser hp(std::move(a), cot::http_parser::server);
        feed(b, wire, slowly).detach();
        auto r1 = co_await cot::attempt(hp.receive(), cot::after(2s));
        assert(r1 && r1->ok() && r1->method() == HTTP_GET && r1->url() == "/first");
        auto r2 = co_await cot::attempt(hp.receive(), cot::after(2s));
        assert(r2 && r2->ok() && r2->method() == HTTP_POST && r2->url() == "/p1");
        assert(r2->body() == "hello");
        auto r3 = co_await cot::attempt(hp.receive(), cot::after(2s));
        assert(r3 && r3->ok() && r3->method() == HTTP_POST && r3->url() == "/p2");
        assert(r3->body() == "world!!");
        auto r4 = co_await cot::attempt(hp.receive(), cot::after(2s));
        assert(r4 && r4->ok() && r4->method() == HTTP_GET && r4->url() == "/last");
        assert(hp.receive_buffer().empty());
    }
}


// TEST: The http_message(string_view method, ...) constructor parses method
// names and rejects unknown ones.
cot::task<> test_method_string_ctor() {
    {
        cot::http_message m("GET", "/foo");
        assert(m.ok());
        assert(m.method() == HTTP_GET);
        assert(std::strcmp(m.method_name(), "GET") == 0);
        assert(m.url() == "/foo");
        assert(m.http_major() == 1);
        assert(m.http_minor() == 1);
        assert(m.status_code() == 200);
    }
    {
        cot::http_message m("POST", "/p");
        assert(m.method() == HTTP_POST);
        assert(m.url() == "/p");
    }
    {
        cot::http_message m("PUT", "/q");
        assert(m.method() == HTTP_PUT);
    }
    {
        cot::http_message m("DELETE", "/r");
        assert(m.method() == HTTP_DELETE);
    }
    {
        cot::http_message m("HEAD", "/h");
        assert(m.method() == HTTP_HEAD);
    }
    {
        cot::http_message m("OPTIONS", "/o");
        assert(m.method() == HTTP_OPTIONS);
    }
    {
        cot::http_message m("PATCH", "/x");
        assert(m.method() == HTTP_PATCH);
    }
    {
        cot::http_message m("CONNECT", "example.com:443");
        assert(m.method() == HTTP_CONNECT);
        assert(m.url() == "example.com:443");
    }

    // Default URL argument.
    {
        cot::http_message m("GET");
        assert(m.method() == HTTP_GET);
        assert(m.url() == "");
    }

    // string_view that is not null-terminated and only views a prefix of a
    // larger buffer must still parse correctly.
    {
        const char* buf = "GETSOMETHING";
        std::string_view sv(buf, 3);
        cot::http_message m(sv, "/y");
        assert(m.method() == HTTP_GET);
        assert(m.url() == "/y");
    }

    // A std::string overload selection check: passing a const char* should
    // resolve to the string_view constructor (not, e.g., the unsigned status
    // code constructor).
    {
        const char* name = "POST";
        cot::http_message m(name, "/z");
        assert(m.method() == HTTP_POST);
        assert(m.url() == "/z");
    }

    // Unknown method names must throw std::invalid_argument.
    auto expect_throw = [](std::string_view name) {
        bool threw = false;
        try {
            cot::http_message m(name, "/");
            (void) m;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    };
    expect_throw("BOGUS");
    expect_throw("FOO");
    // Empty input is not a token.
    expect_throw("");
    // Token rule rejects '@' as a method-name character.
    expect_throw("G@T");
    // Whitespace and other non-tchar characters must be rejected, even when
    // the leading bytes spell a valid method.
    expect_throw("GET ");
    expect_throw(" GET");
    expect_throw("GET/foo");
    expect_throw("GET\tFOO");

    co_return;
}


// TEST: Query-string parsing: has_search_param, search_param, and iteration.
cot::task<> test_search_param() {
    {
        cot::http_message m("GET", "/path?a=1&b=hello&c=2");
        assert(m.has_search_param("a"));
        assert(m.has_search_param("b"));
        assert(m.has_search_param("c"));
        assert(!m.has_search_param("d"));
        assert(m.search_param("a") == "1");
        assert(m.search_param("b") == "hello");
        assert(m.search_param("c") == "2");
        assert(m.search_param("d") == "");
    }

    // Percent-encoding in keys and values; '+' decodes as space.
    {
        cot::http_message m("GET", "/p?hello%20world=a+b&plus=1%2B1");
        assert(m.has_search_param("hello world"));
        assert(m.search_param("hello world") == "a b");
        assert(m.search_param("plus") == "1+1");
    }

    // key= (empty value) and bare key (no '=').
    {
        cot::http_message m("GET", "/p?empty=&bare&full=x");
        assert(m.has_search_param("empty"));
        assert(m.search_param("empty") == "");
        assert(m.has_search_param("bare"));
        assert(m.search_param("bare") == "");
        assert(m.search_param("full") == "x");
    }

    // No query string at all.
    {
        cot::http_message m("GET", "/path");
        assert(!m.has_search_param("a"));
        assert(m.search_param("a") == "");
    }

    // Empty query string ("?" with nothing after).
    {
        cot::http_message m("GET", "/path?");
        assert(!m.has_search_param("a"));
        assert(m.search_param("a") == "");
    }

    // Fragment after query string is not part of search params.
    {
        cot::http_message m("GET", "/p?a=1&b=2#frag");
        assert(m.search_param("a") == "1");
        assert(m.search_param("b") == "2");
        assert(!m.has_search_param("frag"));
    }

    // First match wins for repeated keys.
    {
        cot::http_message m("GET", "/p?a=first&a=second");
        assert(m.search_param("a") == "first");
    }

    // Iteration order matches the URL order; both params visited exactly once.
    {
        cot::http_message m("GET", "/p?a=1&b=2&c=3");
        std::string keys, values;
        for (auto it = m.search_param_begin(); it != m.search_param_end(); ++it) {
            keys += it->first;
            values += it->second;
        }
        assert(keys == "abc");
        assert(values == "123");
    }

    // Iterator range over a URL with no query string is empty.
    {
        cot::http_message m("GET", "/path");
        assert(m.search_param_begin() == m.search_param_end());
    }

    co_return;
}


// TEST: Received header values are trimmed of surrounding whitespace;
// internal whitespace is preserved.
cot::task<> test_header_value_ows() {
    auto req = co_await parse("GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "H-Leading:    value\r\n"
        "H-Trailing: value    \r\n"
        "H-Both:    value    \r\n"
        "H-Tabs:\t\tvalue\t\t\r\n"
        "H-Mixed: \t value \t \r\n"
        "H-Internal: foo bar  baz\r\n"
        "H-Empty:\r\n"
        "H-OWS-Only:    \r\n"
        "\r\n");
    assert(req.header("h-leading") == "value");
    assert(req.header("h-trailing") == "value");
    assert(req.header("h-both") == "value");
    assert(req.header("h-tabs") == "value");
    assert(req.header("h-mixed") == "value");
    assert(req.header("h-internal") == "foo bar  baz");
    assert(req.header("h-empty") == "" && req.has_header("h-empty"));
    assert(req.header("h-ows-only") == "" && req.has_header("h-ows-only"));
}


// TEST: header(key, value) trims surrounding whitespace from `value`.
cot::task<> test_header_setter_ows() {
    cot::http_message m;
    m.header("X-Plain", "value")
     .header("X-Spaced", "   value   ")
     .header("X-Tabs", "\tvalue\t")
     .header("X-Empty", "")
     .header("X-OWS-Only", "   \t  ");

    auto get = [&](const char* name) {
        assert(m.has_header(name));
        return m.header(name);
    };
    assert(get("X-Plain") == "value");
    assert(get("X-Spaced") == "value");
    assert(get("X-Tabs") == "value");
    assert(get("X-Empty") == "");
    assert(get("X-OWS-Only") == "");

    co_return;
}


static_assert(std::ranges::forward_range<cot::strings::http_value_list>);
static_assert(std::ranges::forward_range<cot::strings::http_parameter_list>);

// TEST: http_value_list splits comma-separated header values.
// TEST: url_view splits absolute and origin-form URLs
cot::task<> test_url_view() {
    using cot::strings::url_view;
    {
        url_view u("https://u:p@[::1]:8443/a/b?x=1?y#h?#z");
        assert(u.ok() && u.is_absolute());
        assert(u.scheme() == "https");
        assert(u.authority() == "u:p@[::1]:8443");
        assert(u.host_port() == "[::1]:8443");
        assert(u.host() == "[::1]" && u.port() == "8443");
        assert(u.path() == "/a/b" && u.search() == "?x=1?y" && u.hash() == "#h?#z");
    }
    {
        url_view u("http://example.com");
        assert(u.ok() && u.host() == "example.com" && u.port() == "");
        assert(u.path() == "" && u.search() == "" && u.hash() == "");
    }
    {
        url_view u("HTTP://h:80?q");
        assert(u.ok() && u.scheme() == "HTTP" && u.host_port() == "h:80");
        assert(u.path() == "" && u.search() == "?q");
    }
    {
        url_view u("/p%20q?a=b#c");
        assert(u.ok() && !u.is_absolute() && u.authority() == "");
        assert(u.path() == "/p%20q" && u.search() == "?a=b" && u.hash() == "#c");
    }
    {
        url_view u("//not/authority");             // origin-form path
        assert(u.ok() && !u.is_absolute() && u.path() == "//not/authority");
    }
    {
        url_view u("http://h:/x");                  // empty port
        assert(u.ok() && u.host() == "h" && u.port() == "");
    }
    for (const char* bad : {"", "x", "*", "h:80", "1http://h/", "http:/h",
                            "http://h:65536/", "http://h:8x/", "http://[::1/",
                            "http://[::1]x/", "http://h/a b", "/a b",
                            "/a\x01", "http://h\"/", "/\xc3\xa9"}) {
        url_view u(bad);
        assert(!u.ok() && u.path() == "" && u.host() == "");
    }
    std::cerr << "url_view: ok\n";
    co_return;
}

// TEST: http_message parses absolute-form request targets
cot::task<> test_absolute_form() {
    auto r = co_await parse("GET http://example.com:8080/a/%7Eb?x=1&y=2 HTTP/1.1\r\n"
                            "Host: example.com:8080\r\n\r\n");
    assert(r.url() == "http://example.com:8080/a/%7Eb?x=1&y=2");
    assert(r.has_valid_url());
    assert(r.path() == "/a/%7Eb" && r.search() == "?x=1&y=2");
    assert(r.search_param("y") == "2");
    assert(r.decoded_path() == "/a/~b");
    r = co_await parse("GET http://example.com?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    assert(r.has_valid_url() && r.path() == "/" && r.search_param("q") == "1");
    // origin-form is unchanged
    cot::http_message m(HTTP_GET, "/x?a=1#h");
    assert(m.has_valid_url() && m.path() == "/x" && m.search() == "?a=1"
           && m.hash() == "#h");
    m.url("not a url");
    assert(!m.has_valid_url() && m.path() == "" && m.search_param("a") == "");
    std::cerr << "absolute_form: ok\n";
}

cot::task<> test_http_value_list() {
    using cot::strings::http_value_list;

    // Basic three items, no extra whitespace.
    {
        http_value_list lst("a, b, c");
        auto it = lst.begin(), end = lst.end();
        assert(it != end); assert(*it == "a"); ++it;
        assert(it != end); assert(*it == "b"); ++it;
        assert(it != end); assert(*it == "c"); ++it;
        assert(it == end);
    }

    // Surrounding OWS (SP/HTAB) is trimmed from each entry.
    {
        http_value_list lst("  a  ,\t b\t ,  c");
        auto it = lst.begin(), end = lst.end();
        assert(*it == "a"); ++it;
        assert(*it == "b"); ++it;
        assert(*it == "c"); ++it;
        assert(it == end);
    }

    // Empty entries (`,,` / leading or trailing `,`) are skipped per
    // `1#element` semantics.
    {
        http_value_list lst(",, a, , , b ,,");
        auto it = lst.begin(), end = lst.end();
        assert(*it == "a"); ++it;
        assert(*it == "b"); ++it;
        assert(it == end);
    }

    // Quoted-strings shield embedded commas from the splitter.
    {
        http_value_list lst("a, \"b, c\", d");
        auto it = lst.begin(), end = lst.end();
        assert(*it == "a"); ++it;
        assert(*it == "\"b, c\""); ++it;
        assert(*it == "d"); ++it;
        assert(it == end);
    }

    // Backslash-escapes inside a quoted-string don't terminate the quote.
    {
        http_value_list lst("\"a\\\"b\", x");
        auto it = lst.begin(), end = lst.end();
        assert(*it == "\"a\\\"b\""); ++it;
        assert(*it == "x"); ++it;
        assert(it == end);
    }

    // Empty / OWS-only inputs yield no entries.
    {
        http_value_list empty("");
        assert(empty.begin() == empty.end());
        http_value_list blanks("   ,  , \t,");
        assert(blanks.begin() == blanks.end());
    }

    // icontains is case-insensitive and respects token boundaries.
    {
        http_value_list lst("Upgrade, keep-alive");
        assert(lst.icontains("upgrade"));
        assert(lst.icontains("UPGRADE"));
        assert(lst.icontains("Keep-Alive"));
        assert(!lst.icontains("close"));
        assert(!lst.icontains("up"));     // not a prefix match
    }

    co_return;
}


// TEST: http_parameter_list splits semicolon-separated parameters.
cot::task<> test_http_parameter_list() {
    using cot::strings::http_parameter_list;

    // Basic `name=value` pairs.
    {
        std::string src = "foo=bar; baz=qux";
        http_parameter_list lst(src);
        auto it = lst.begin(), end = lst.end();
        assert(it != end);
        assert(it.name() == "foo");
        assert(it.raw_value() == "bar");
        assert(it.span() == "foo=bar");
        ++it;
        assert(it != end);
        assert(it.name() == "baz");
        assert(it.raw_value() == "qux");
        ++it;
        assert(it == end);
    }

    // Standalone parameters (no `=`): name() and raw_value() are the whole
    // token, and has_value() is false.
    {
        std::string src = "permessage-deflate; client_no_context_takeover";
        http_parameter_list lst(src);
        auto it = lst.begin(), end = lst.end();
        assert(it != end);
        assert(it.name() == "permessage-deflate");
        assert(it.raw_value() == "permessage-deflate" && !it.has_value());
        ++it;
        assert(it != end);
        assert(it.name() == "client_no_context_takeover");
        ++it;
        assert(it == end);
    }

    // Mixed standalone + name=value (the permessage-deflate shape).
    {
        std::string src = "permessage-deflate; client_max_window_bits=15";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "permessage-deflate");
        ++it;
        assert(it.name() == "client_max_window_bits");
        assert(it.raw_value() == "15");
        ++it;
        assert(it == lst.end());
    }

    // `;` inside a quoted-string doesn't split.
    {
        std::string src = "title=\"hello; world\"; charset=utf-8";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "title");
        assert(it.raw_value() == "\"hello; world\"");
        assert(it.value() == "hello; world");      // unquoted
        ++it;
        assert(it.name() == "charset");
        assert(it.raw_value() == "utf-8");
        assert(it.value() == "utf-8");
        ++it;
        assert(it == lst.end());
    }

    // Trailing OWS is trimmed; empty/OWS-only segments are skipped.
    {
        std::string src = ";; foo=bar  ;  ;baz=qux ;";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "foo");
        assert(it.raw_value() == "bar");
        ++it;
        assert(it.name() == "baz");
        assert(it.raw_value() == "qux");
        ++it;
        assert(it == lst.end());
    }

    // An empty value, with or without trailing whitespace.
    {
        std::string src = "foo=; bar=  ; baz=x";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "foo" && it.has_value() && it.raw_value() == "");
        assert(it.span() == "foo=" && it.value() == "");
        ++it;
        assert(it.name() == "bar" && it.has_value() && it.raw_value() == "");
        ++it;
        assert(it.name() == "baz" && it.raw_value() == "x");
        ++it;
        assert(it == lst.end());
    }

    // Leading `=` is not a name.
    {
        std::string src = "=x";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "=x" && !it.has_value());
    }

    // Whitespace around `=` defeats the name/value split (strict RFC).
    {
        std::string src = "foo = bar";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "foo = bar");        // no name recognized
        ++it;
        assert(it == lst.end());
        http_parameter_list lst2("foo= bar; baz =qux");
        it = lst2.begin();
        assert(it.name() == "foo= bar" && !it.has_value());
        ++it;
        assert(it.name() == "baz =qux" && !it.has_value());
    }

    // unquote(): strips surrounding quotes and decodes backslash escapes.
    {
        assert(cot::strings::http_unquote("plain") == "plain");
        assert(cot::strings::http_unquote("\"hello\"") == "hello");
        assert(cot::strings::http_unquote("\"a\\\"b\"") == "a\"b");
        assert(cot::strings::http_unquote("\"\"") == "");
        assert(cot::strings::http_unquote("") == "");
    }

    // Empty input.
    {
        http_parameter_list empty("");
        assert(empty.begin() == empty.end());
    }

    // Complex empty input.
    {
        http_parameter_list empty(" ; ;;;;; ; ;; ;;; ;;;");
        assert(empty.begin() == empty.end());
    }

    // Composition: Sec-WebSocket-Extensions-style header with two
    // offerings, each a parameterized token.
    {
        std::string hdr =
            "permessage-deflate; client_no_context_takeover, x-foo; opt=1";
        cot::strings::http_value_list outer(hdr);

        auto oit = outer.begin();
        assert(oit != outer.end());
        std::string first(*oit);
        assert(first == "permessage-deflate; client_no_context_takeover");
        ++oit;
        std::string second(*oit);
        assert(second == "x-foo; opt=1");
        ++oit;
        assert(oit == outer.end());

        http_parameter_list p0(first);
        auto pit = p0.begin();
        assert(pit.raw_value() == "permessage-deflate");
        ++pit;
        assert(pit.raw_value() == "client_no_context_takeover");
        ++pit;
        assert(pit == p0.end());

        http_parameter_list p1(second);
        pit = p1.begin();
        assert(pit.raw_value() == "x-foo");
        ++pit;
        assert(pit.name() == "opt");
        assert(pit.raw_value() == "1");
        ++pit;
        assert(pit == p1.end());
    }

    co_return;
}



// TEST: The COTAMERIO.md proxy recipe removes hop-by-hop headers.
cot::task<> test_hop_by_hop() {
    using cot::strings::http_value_list;
    auto req = co_await parse("POST /x HTTP/1.1\r\nHost: x\r\n"
        "Connection: keep-alive, X-Secret\r\nKeep-Alive: timeout=5\r\n"
        "X-Secret: 1\r\nX-Public: 2\r\nTE: trailers\r\n"
        "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    std::string conn = req.header("connection");
    http_value_list hop("connection, keep-alive, proxy-connection, te, trailer, upgrade");
    req.remove_header_if([&](std::string_view name, std::string_view) {
        return hop.icontains(name) || http_value_list(conn).icontains(name);
    });
    auto out = co_await serialize(std::move(req), true);
    assert(out == "POST /x HTTP/1.1\r\nHost: x\r\nX-Public: 2\r\n"
                  "Content-Length: 5\r\n\r\nhello");
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
        t.promise_ptr()->result();      // rethrow if the test threw
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
    run("dropped_ticket", test_dropped_ticket);
    run("close_delimited", test_close_delimited);
    run("receive_after_error", test_receive_after_error);
    run("remove_header", test_remove_header);
    run("forward_framing", test_forward_framing);
    run("pipelining", test_pipelining);
    run("method_string_ctor", test_method_string_ctor);
    run("search_param", test_search_param);
    run("header_value_ows", test_header_value_ows);
    run("header_setter_ows", test_header_setter_ows);
    run("url_view", test_url_view);
    run("absolute_form", test_absolute_form);
    run("http_value_list", test_http_value_list);
    run("http_parameter_list", test_http_parameter_list);
    run("hop_by_hop", test_hop_by_hop);

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
