#pragma once
#include "cotamer/config.hh"
#include "cotamer/io.hh"
#include "llhttp.h"
#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <ctime>
#if COTAMER_HAVE_NLOHMANN_JSON
# include <nlohmann/json_fwd.hpp>
#endif
namespace cotamer {
template <typename Stream> class http_parser;
namespace http {
class parser_base;
enum parser_type { client, server };
}

namespace strings {
bool ieq(const char* a, const char* b, size_t n) noexcept;
inline constexpr bool ieq(std::string_view s1, std::string_view s2) noexcept {
    return s1.size() == s2.size() && ieq(s1.data(), s2.data(), s1.size());
}

// HTTP header value iteration (RFC 9110 §5.6). A header field value can carry
// a comma-separated list of values; each value can carry a primary token plus
// semicolon-separated parameters of the form `name` or `name=value`.
//
//   for (auto v : strings::http_header_values(field_value)) {
//       if (strings::ieq(v.name(), "permessage-deflate")) {
//           for (auto p : v.params()) {
//               // p.name, p.value (both trimmed of OWS; value empty if no '=')
//           }
//       }
//   }
//
// All views point into the input string; the input must outlive the iteration.
// Quoted-string parameter values (e.g. `q="0.5"`) are not unquoted; treat the
// returned `value` as the raw text between '=' and ';' (or end), trimmed.

struct http_header_param {
    std::string_view name;
    std::string_view value;        // empty if no '='
};

class http_header_param_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = http_header_param;
    using difference_type = std::ptrdiff_t;
    using reference = http_header_param;
    using pointer = void;

    http_header_param_iterator() noexcept = default;
    explicit http_header_param_iterator(std::string_view rest) noexcept;

    http_header_param operator*() const noexcept { return current_; }
    http_header_param_iterator& operator++() noexcept;
    bool operator==(const http_header_param_iterator& x) const noexcept {
        return current_.name.empty() && x.current_.name.empty();
    }

private:
    std::string_view rest_{};
    http_header_param current_{};
};

class http_header_param_range {
public:
    explicit http_header_param_range(std::string_view s) noexcept : str_(s) {}
    http_header_param_iterator begin() const noexcept {
        return http_header_param_iterator{str_};
    }
    http_header_param_iterator end() const noexcept { return {}; }
private:
    std::string_view str_;
};

class http_header_value {
public:
    constexpr explicit http_header_value(std::string_view raw) noexcept : raw_(raw) {}

    // Text up to the first ';' (or whole value if no params), trimmed.
    std::string_view name() const noexcept;
    // Range over the ';'-separated parameters after the name.
    http_header_param_range params() const noexcept;
    // The full value (everything between commas), trimmed.
    constexpr std::string_view text() const noexcept { return raw_; }

private:
    std::string_view raw_;
};

class http_header_value_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = http_header_value;
    using difference_type = std::ptrdiff_t;
    using reference = http_header_value;
    using pointer = void;

    http_header_value_iterator() noexcept = default;
    explicit http_header_value_iterator(std::string_view rest) noexcept;

    http_header_value operator*() const noexcept { return http_header_value{current_}; }
    http_header_value_iterator& operator++() noexcept;
    bool operator==(const http_header_value_iterator& x) const noexcept {
        return current_.empty() && x.current_.empty();
    }

private:
    std::string_view rest_{};
    std::string_view current_{};
};

class http_header_value_range {
public:
    explicit http_header_value_range(std::string_view s) noexcept : str_(s) {}
    http_header_value_iterator begin() const noexcept {
        return http_header_value_iterator{str_};
    }
    http_header_value_iterator end() const noexcept { return {}; }
private:
    std::string_view str_;
};

inline http_header_value_range http_header_values(std::string_view header) noexcept {
    return http_header_value_range{header};
}

}  // namespace strings

struct http_opair {
    unsigned name1;
    unsigned name2;
    unsigned value1;
    unsigned value2;

    inline constexpr std::string_view name(const char* base) const noexcept {
        return {base + name1, name2 - name1};
    }
    inline constexpr std::string_view value(const char* base) const noexcept {
        return {base + value1, value2 - value1};
    }

    inline bool name_ieq(const char* base, std::string_view str) const noexcept {
        return strings::ieq(name(base), str);
    }
};

class http_header_iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;

    http_header_iterator() = default;
    http_header_iterator(const char* base, const http_opair* pair)
        : base_(base), pair_(pair) {
    }
    http_header_iterator(const http_header_iterator&) = default;
    http_header_iterator(http_header_iterator&&) = default;
    http_header_iterator& operator=(const http_header_iterator&) = default;
    http_header_iterator& operator=(http_header_iterator&&) = default;

    constexpr std::string_view name() const noexcept {
        return std::string_view{base_ + pair_->name1, pair_->name2 - pair_->name1};
    }
    bool name_ieq(std::string_view str) const noexcept {
        return pair_->name_ieq(base_, str);
    }

    constexpr std::string_view value() const noexcept {
        return std::string_view{base_ + pair_->value1, pair_->value2 - pair_->value1};
    }

    auto operator<=>(const http_header_iterator& x) const noexcept {
        return pair_ <=> x.pair_;
    }
    bool operator==(const http_header_iterator& x) const noexcept {
        return pair_ == x.pair_;
    }

    http_header_iterator& operator++() { ++pair_; return *this; }
    http_header_iterator operator++(int) { auto tmp{*this}; ++pair_; return tmp; }

    http_header_iterator& operator--() { --pair_; return *this; }
    http_header_iterator operator--(int) { auto tmp{*this}; --pair_; return tmp; }

    difference_type operator-(const http_header_iterator& x) const noexcept {
        return pair_ - x.pair_;
    }

private:
    const char* base_;
    const http_opair* pair_;
};


class http_message {
public:
    typedef http_header_iterator header_iterator;

    inline http_message(llhttp_method method = HTTP_GET,
                        std::string url = std::string());
    http_message(http_message&&) = default;
    http_message& operator=(http_message&&) = default;
    inline http_message(const http_message&);
    inline http_message& operator=(const http_message&);

    inline constexpr bool ok() const;
    explicit inline constexpr operator bool() const;
    inline constexpr bool operator!() const;
    inline constexpr llhttp_errno error() const;
    inline const char* error_name() const;

    inline constexpr unsigned http_major() const;
    inline constexpr unsigned http_minor() const;
    inline constexpr unsigned status_code() const;
    inline const std::string& status_message() const;
    inline constexpr llhttp_method method() const;
    inline const char* method_name() const;

    // Examine headers
    inline bool has_header(std::string_view name) const;
    header_iterator find_header(std::string_view name) const;
    std::string header(std::string_view name) const;
    inline header_iterator header_begin() const;
    inline header_iterator header_end() const;

    // Examine URL and URL parts
    inline constexpr const std::string& url() const;   // `/path?a=b&c=d#hash`
    inline bool has_valid_url() const;
    inline bool has_path() const;
    inline bool has_search() const;
    inline bool has_hash() const;
    inline std::string_view path() const;      // → `/path`
    inline std::string_view search() const;    // → `?a=b&c=d`
    inline std::string_view hash() const;      // → `#hash`
    bool has_search_param(std::string_view name) const;
    std::string_view search_param(std::string_view name) const;
    inline header_iterator search_param_begin() const;
    inline header_iterator search_param_end() const;

    // Examine body
    inline const std::string& body() const;

    // Modify message
    inline http_message& clear();
    void add_header(std::string key, std::string value);
    inline void add_header(std::string key, size_t value);

    inline http_message& http_major(unsigned v);
    inline http_message& http_minor(unsigned v);
    inline http_message& error(llhttp_errno e);
    inline http_message& status_code(unsigned code);
    inline http_message& status_code(unsigned code, std::string message);
    inline http_message& method(llhttp_method method);
    inline http_message& url(std::string url);
    inline http_message& header(std::string key, std::string value);
    inline http_message& header(std::string key, size_t value);
    inline http_message& date_header(std::string key, time_t value);

    inline http_message& body(std::string body);
    inline http_message& append_body(const std::string& x);
#if COTAMER_HAVE_NLOHMANN_JSON
    http_message& body(const nlohmann::json& j);
#endif

    static const char* default_status_message(unsigned code);

private:
    enum {
        info_url = 1, info_params = 2
    };

    struct info_type {
        unsigned flags = 0;
        std::pair<unsigned, unsigned> f[3];
        std::string qurl;
        std::vector<http_opair> qpairs;
        inline info_type()
            : flags(0) {
        }
    };

    unsigned char major_;
    unsigned char minor_;
    unsigned char method_;
    unsigned char error_;
    unsigned short status_code_;
    unsigned upgrade_ : 1;
    unsigned has_body_ : 1;

    std::string url_;
    std::string status_message_;
    std::string headers_;
    std::vector<http_opair> hpairs_;
    std::string body_;
    mutable std::unique_ptr<info_type> info_;

    inline void kill_info(unsigned f) const;
    inline info_type& info(unsigned f) const;
    void make_info(unsigned f) const;
    inline bool has_url_field(int field) const;
    inline std::string_view url_field(int field) const;
    void do_clear();
    friend class http::parser_base;
};

namespace http {
class parser_base {
public:
    using ticket_type = mutex_event<false>;

    parser_base(parser_type direction = server, std::string host = std::string());

    inline bool ok() const;
    inline constexpr llhttp_errno error() const;
    inline const char* error_name() const;

    void clear();

    inline const std::string& host() const;
    inline void set_host(const std::string& host);
    inline void clear_host();

    inline bool should_keep_alive() const;
    inline void clear_should_keep_alive();

    // Data buffered after parsing the HTTP message
    inline const std::string& receive_buffer() const;
    inline std::string& receive_buffer();

protected:
    static constexpr size_t bufsize = 8192;

    ::llhttp_t hp_;
    mutex m_[2];
    std::string receive_buffer_;
    std::string host_;

    enum { state_unknown, state_header_name, state_http_header_value, state_done };

    struct message_data {
        http_message hm;
        int state = state_unknown;
        unsigned name1;
        unsigned name2;
        unsigned value1;
    };

    static const llhttp_settings_t settings;
    static parser_base* get_parser(::llhttp_t* hp);
    static message_data* get_message_data(::llhttp_t* hp);
    static int on_message_begin(::llhttp_t* hp);
    static int on_url(::llhttp_t* hp, const char* s, size_t len);
    static int on_status(::llhttp_t* hp, const char* s, size_t len);
    static int on_header_field(::llhttp_t* hp, const char* s, size_t len);
    static int on_header_field_complete(::llhttp_t* hp);
    static int on_header_value(::llhttp_t* hp, const char* s, size_t len);
    static int on_header_value_complete(::llhttp_t* hp);
    static int on_headers_complete(::llhttp_t* hp);
    static int on_chunk_header(::llhttp_t* hp);
    static int on_body(::llhttp_t* hp, const char* s, size_t len);
    static int on_message_complete(::llhttp_t* hp);
    inline void copy_parser_status(message_data& md);

    bool receive_chunk(message_data&, const char* buf, size_t sz);
    size_t prepare_request(std::string&, http_message&, iovec*);
    size_t prepare_response(std::string&, http_message&, iovec*);
};
}

template <typename Stream>
class basic_http_parser : public http::parser_base {
public:
    http_parser(Stream s, http::parser_type direction = http::server,
                std::string host = std::string());

    inline task<http_message> receive();
    task<http_message> receive(ticket_type);

    task<> send(http_message);
    task<ticket_type> send_request(http_message);
    task<> send_response(http_message);
    task<> send_response_chunk(std::string);
    task<> send_response_end_chunk();

    // Underlying stream
    inline const Stream& stream() const;
    // Release ownership of the underlying fd (e.g., for protocol upgrades)
    inline Stream take_stream();

private:
    Stream stream_;
};

using http_parser = basic_http_parser<cot::fd>;

inline http_message::http_message(llhttp_method method, std::string url)
    : major_(1), minor_(1), method_(method), error_(HPE_OK),
      status_code_(200), upgrade_(0), has_body_(0), url_(std::move(url)) {
}

inline http_message::http_message(const http_message& x)
    : major_(x.major_), minor_(x.minor_), method_(x.method_), error_(x.error_),
      status_code_(x.status_code_), upgrade_(x.upgrade_), has_body_(x.has_body_),
      url_(x.url_), status_message_(x.status_message_), headers_(x.headers_),
      body_(x.body_) {
}

inline http_message& http_message::operator=(const http_message& x) {
    if (&x != this) {
        major_ = x.major_;
        minor_ = x.minor_;
        method_ = x.method_;
        error_ = x.error_;
        status_code_ = x.status_code_;
        upgrade_ = x.upgrade_;
        has_body_ = x.has_body_;
        url_ = x.url_;
        status_message_ = x.status_message_;
        headers_ = x.headers_;
        hpairs_ = x.hpairs_;
        body_ = x.body_;
        if (info_) {
            info_->flags = 0;
        }
    }
    return *this;
}

inline void http_message::kill_info(unsigned fl) const {
    if (info_) {
        info_->flags &= ~fl;
    }
}

inline constexpr unsigned http_message::http_major() const {
    return major_;
}

inline constexpr unsigned http_message::http_minor() const {
    return minor_;
}

inline constexpr bool http_message::ok() const {
    return error_ == HPE_OK;
}

inline constexpr http_message::operator bool() const {
    return ok();
}

inline constexpr bool http_message::operator!() const {
    return !ok();
}

inline constexpr llhttp_errno http_message::error() const {
    return (llhttp_errno) error_;
}

inline const char* http_message::error_name() const {
    return llhttp_errno_name((llhttp_errno) error_);
}

inline constexpr unsigned http_message::status_code() const {
    return status_code_;
}

inline const std::string& http_message::status_message() const {
    return status_message_;
}

inline constexpr llhttp_method http_message::method() const {
    return (llhttp_method) method_;
}

inline const char* http_message::method_name() const {
    return llhttp_method_name((llhttp_method) method_);
}

inline constexpr const std::string& http_message::url() const {
    return url_;
}

inline bool http_message::has_header(std::string_view name) const {
    return find_header(name) != header_end();
}

inline const std::string& http_message::body() const {
    return body_;
}

inline bool http_message::has_url_field(int field) const {
    auto& inf = info(info_url);
    return inf.f[field].first != inf.f[field].second;
}

inline std::string_view http_message::url_field(int field) const {
    auto& inf = info(info_url);
    if (inf.f[field].first != inf.f[field].second) {
        return std::string_view(url_.data() + inf.f[field].first,
                                url_.data() + inf.f[field].second);
    }
    return std::string_view();
}

inline bool http_message::has_valid_url() const {
    return has_url_field(0);
}

inline std::string_view http_message::path() const {
    return url_field(0);
}

inline bool http_message::has_search() const {
    return has_url_field(1);
}

inline std::string_view http_message::search() const {
    return url_field(1);
}

inline bool http_message::has_hash() const {
    return has_url_field(2);
}

inline std::string_view http_message::hash() const {
    return url_field(2);
}

inline http_message& http_message::http_major(unsigned v) {
    major_ = v;
    return *this;
}

inline http_message& http_message::http_minor(unsigned v) {
    minor_ = v;
    return *this;
}

inline http_message& http_message::clear() {
    do_clear();
    return *this;
}

inline http_message& http_message::error(llhttp_errno e) {
    error_ = e;
    return *this;
}

inline http_message& http_message::status_code(unsigned code) {
    status_code_ = code;
    status_message_ = std::string();
    return *this;
}

inline http_message& http_message::status_code(unsigned code, std::string message) {
    status_code_ = code;
    status_message_ = std::move(message);
    return *this;
}

inline http_message& http_message::method(llhttp_method method) {
    method_ = (unsigned) method;
    return *this;
}

inline http_message& http_message::url(std::string url) {
    url_ = std::move(url);
    kill_info(info_url | info_params);
    return *this;
}

inline void http_message::add_header(std::string key, size_t value) {
    add_header(std::move(key), std::to_string(value));
}

inline http_message& http_message::header(std::string key, std::string value) {
    add_header(std::move(key), std::move(value));
    return *this;
}

inline http_message& http_message::header(std::string key, size_t value) {
    add_header(std::move(key), value);
    return *this;
}

inline http_message& http_message::date_header(std::string key, time_t value) {
    char buf[128];
    // XXX current locale
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&value));
    add_header(std::move(key), std::string(buf));
    return *this;
}

inline http_message& http_message::body(std::string body) {
    body_ = std::move(body);
    has_body_ = 1;
    return *this;
}

inline http_message& http_message::append_body(const std::string& x) {
    body_ += x;
    has_body_ = 1;
    return *this;
}

inline http_message::info_type& http_message::info(unsigned f) const {
    if (!info_ || (info_->flags & f) != f) {
        make_info(f);
    }
    return *info_.get();
}

inline http_message::header_iterator http_message::header_begin() const {
    return http_header_iterator{headers_.data(), hpairs_.data()};
}

inline http_message::header_iterator http_message::header_end() const {
    return http_header_iterator{headers_.data(), hpairs_.data() + hpairs_.size()};
}

inline http_message::header_iterator http_message::search_param_begin() const {
    auto& inf = info(info_params);
    return http_header_iterator{inf.qurl.data(), inf.qpairs.data()};
}

inline http_message::header_iterator http_message::search_param_end() const {
    auto& inf = info(info_params);
    return http_header_iterator{inf.qurl.data(), inf.qpairs.data() + inf.qpairs.size()};
}

namespace http {
inline bool parser_base::ok() const {
    return hp_.error == (unsigned) HPE_OK;
}

inline constexpr llhttp_errno parser_base::error() const {
    return (llhttp_errno) hp_.error;
}

inline const char* parser_base::error_name() const {
    return llhttp_errno_name((llhttp_errno) hp_.error);
}

inline const std::string& parser_base::host() const {
    return host_;
}

inline void parser_base::set_host(const std::string& host) {
    host_ = host;
}

inline void parser_base::clear_host() {
    host_.clear();
}

inline bool parser_base::should_keep_alive() const {
    return llhttp_should_keep_alive(&hp_);
}

inline void parser_base::clear_should_keep_alive() {
    hp_.flags = (hp_.flags & ~F_CONNECTION_KEEP_ALIVE) | F_CONNECTION_CLOSE;
}

inline const std::string& parser_base::receive_buffer() const {
    return receive_buffer_;
}

inline std::string& parser_base::receive_buffer() {
    return receive_buffer_;
}
}

template <typename Stream>
http_parser<Stream>::http_parser(Stream stream, http::parser_type direction, std::string host)
    : parser_base(direction, std::move(host)), stream_(std::move(stream)) {
}

template <typename Stream>
task<http_message> http_parser<Stream>::receive(ticket_type ticket) {
    assert(ticket.mutex() == &m_[0]);
    char stackbuf[bufsize];
    message_data md;
    md.hm.status_code(0);
    unique_lock guard(co_await ticket);
    hp_.data = &md;

    const char* buf;
    size_t nr;
    do {
        if (receive_buffer_.empty()) {
            buf = stackbuf;
            auto nx = co_await recv_once(stream_, stackbuf, sizeof(stackbuf));
            nr = nx.value_or(0);
        } else {
            // Drain bytes left from the previous call (pipelined data, or a
            // partial frame we couldn't process in one llhttp_execute).
            buf = receive_buffer_.data();
            nr = receive_buffer_.size();
        }
    } while (receive_chunk(md, buf, nr));
    co_return std::move(md.hm);
}

template <typename Stream>
inline task<http_message> http_parser<Stream>::receive() {
    return receive(m_[0].lock());
}

template <typename Stream>
task<> http_parser<Stream>::send(http_message m) {
    assert(hp_.type == (int) HTTP_RESPONSE || hp_.type == (int) HTTP_REQUEST);
    if (hp_.type == (int) HTTP_RESPONSE) {
        co_await send_request(std::move(m));
    } else {
        co_await send_response(std::move(m));
    }
}

template <typename Stream>
task<http::parser_base::ticket_type> http_parser<Stream>::send_request(http_message m) {
    std::string urlline;
    iovec iov[3];
    size_t iovcnt = prepare_request(urlline, m, iov);

    // lock for writing, then obtain read ticket
    unique_lock guard(co_await m_[1].lock());
    auto ticket = m_[0].lock();

    co_await sendv(stream_, iov, iovcnt);
    co_return std::move(ticket);
}

template <typename Stream>
task<> http_parser<Stream>::send_response(http_message m) {
    std::string first;
    iovec iov[3];
    size_t iovcnt = prepare_response(first, m, iov);
    unique_lock guard(co_await m_[1].lock());
    co_await sendv(stream_, iov, iovcnt);
}

template <typename Stream>
task<> http_parser<Stream>::send_response_chunk(std::string str) {
    std::string lenline = std::format("{}\r\n", str.length());
    iovec iov[3];
    iov[0] = iovec{ lenline.data(), lenline.length() };
    iov[1] = iovec{ str.data(), str.length() };
    iov[2] = iovec{ (void*) "\r\n", 2 };
    unique_lock guard(co_await m_[1].lock());
    co_await sendv(stream_, iov, 3);
}

template <typename Stream>
task<> http_parser<Stream>::send_response_end_chunk() {
    using cotamer::send;
    unique_lock guard(co_await m_[1].lock());
    co_await send(stream_, "0\r\n\r\n", 5);
}

template <typename Stream>
inline const Stream& http_parser<Stream>::stream() const {
    return stream_;
}

template <typename Stream>
inline Stream http_parser<Stream>::take_stream() {
    return std::move(stream_);
}

} // namespace cotamer
