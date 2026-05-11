#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "cotamer/http_fields.hh"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
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


// The http_message(string_view, ...) constructor parses the method name into
// an llhttp_method via http_parser::parse_method.
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

    std::cerr << "method_string_ctor: ok\n";
    co_return;
}


// Query-string parsing: has_search_param, search_param, and iteration.
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

    std::cerr << "search_param: ok\n";
    co_return;
}


// Header values returned by http_parser should be OWS-trimmed (RFC 9110
// §5.5: leading/trailing SP and HTAB are not part of the field value).
// Internal whitespace must be preserved; empty values stay empty.
cot::task<> test_header_value_ows() {
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

        assert(req.header("h-leading") == "value");
        assert(req.header("h-trailing") == "value");
        assert(req.header("h-both") == "value");
        assert(req.header("h-tabs") == "value");
        assert(req.header("h-mixed") == "value");
        // Internal whitespace must be preserved verbatim.
        assert(req.header("h-internal") == "foo bar  baz");
        // Empty / OWS-only values normalize to empty.
        assert(req.header("h-empty") == "");
        assert(req.has_header("h-empty"));
        assert(req.header("h-ows-only") == "");
        assert(req.has_header("h-ows-only"));

        server_done.trigger();
    };
    server().detach();

    auto cfd = co_await connect_pair(addr, listener_ready);
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "H-Leading:    value\r\n"
        "H-Trailing: value    \r\n"
        "H-Both:    value    \r\n"
        "H-Tabs:\t\tvalue\t\t\r\n"
        "H-Mixed: \t value \t \r\n"
        "H-Internal: foo bar  baz\r\n"
        "H-Empty:\r\n"
        "H-OWS-Only:    \r\n"
        "\r\n";
    co_await cot::write(cfd, raw.data(), raw.size());

    co_await cot::attempt(cot::task<>{server_done}, cot::after(500ms));
    std::cerr << "header_value_ows: ok\n";
}


// http_message::header(key, value) should also trim OWS from `value`.
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

    std::cerr << "header_setter_ows: ok\n";
    co_return;
}


// Tests for cotamer::strings::http_value_list — comma-separated list
// values (RFC 9110 §5.6.1 `#rule`).
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

    std::cerr << "http_value_list: ok\n";
    co_return;
}


// Tests for cotamer::strings::http_parameter_list — semicolon-separated
// `name[=value]` parameter lists (RFC 9110 §5.6.6).
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

    // Standalone parameters (no `=`): name() is empty, raw_value() holds
    // the whole token.
    {
        std::string src = "permessage-deflate; client_no_context_takeover";
        http_parameter_list lst(src);
        auto it = lst.begin(), end = lst.end();
        assert(it != end);
        assert(it.name() == "permessage-deflate");
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

    // Whitespace around `=` defeats the name/value split (strict RFC).
    {
        std::string src = "foo = bar";
        http_parameter_list lst(src);
        auto it = lst.begin();
        assert(it.name() == "foo = bar");        // no name recognized
        ++it;
        assert(it == lst.end());
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

    std::cerr << "http_parameter_list: ok\n";
    co_return;
}


int main(int argc, char* argv[]) {
    cot::set_clock(cot::clock::real_time);

    int ran = 0;
    auto run = [&](const char* name, auto fn) {
        bool found = argc == 1;
        for (int i = 1; !found && i < argc; ++i) {
            found = std::strcmp(name, argv[i]) == 0;
        }
        if (!found) {
            return;
        }
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
    run("method_string_ctor", test_method_string_ctor);
    run("search_param", test_search_param);
    run("header_value_ows", test_header_value_ows);
    run("header_setter_ows", test_header_setter_ows);
    run("http_value_list", test_http_value_list);
    run("http_parameter_list", test_http_parameter_list);

    if (ran == 0) {
        std::cerr << "No matching tests\n";
        return 1;
    }
    std::cerr << "*** done ***\n";
    return 0;
}
