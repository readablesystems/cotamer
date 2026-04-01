#pragma once
#include "fd.hh"
#include "http_parser.h"
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <ctime>
namespace cotamer {
class http_parser;

struct http_opair {
    unsigned name1;
    unsigned name2;
    unsigned value1;
    unsigned value2;

    inline bool name_eq(const char* base, const char* s, size_t len) const {
        return name2 - name1 == len
            && memcmp(base + name1, s, len) == 0;
    }
    inline bool name_eq(const char* base, std::string_view str) const {
        return name_eq(base, str.data(), str.length());
    }
    inline bool name_eq_case(const char* base, const char* s, size_t len) const {
        return name2 - name1 == len
            && case_eq_prefix(base + name1, s, len);
    }
    inline bool name_eq_case(const char* base, std::string_view str) const {
        return name_eq_case(base, str.data(), str.length());
    }
    inline bool is_content_length(const char* base) const {
        return name_eq_case(base, "content-length", 14);
    }

    static inline bool case_eq_prefix(const char* s1, const char* s2, size_t len) {
        const char* e2 = s2 + len;
        while (s2 != e2) {
            unsigned char d = *s1 ^ *s2;
            if (d != 0
                && (d != 0x20 || (unsigned char) ((*s1 | 0x20) - 'a') >= 26)) {
                return false;
            }
            ++s1;
            ++s2;
        }
        return true;
    }
};

struct http_header_iterator_proxy {
    using value_type = std::pair<std::string_view, std::string_view>;
    value_type pair;
    const value_type* operator->() const noexcept { return &pair; }
};

class http_header_iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<std::string_view, std::string_view>;
    using difference_type = std::ptrdiff_t;
    using reference = value_type;
    using pointer = http_header_iterator_proxy;

    http_header_iterator() = default;
    http_header_iterator(const char* base, const http_opair* pair)
        : base_(base), pair_(pair) {
    }
    http_header_iterator(const http_header_iterator&) = default;
    http_header_iterator(http_header_iterator&&) = default;
    http_header_iterator& operator=(const http_header_iterator&) = default;
    http_header_iterator& operator=(http_header_iterator&&) = default;

    constexpr std::string_view name() const noexcept {
        return std::string_view{base_ + pair_->name1, base_ + pair_->name2};
    }
    bool name_eq(const char* s, size_t len) const noexcept {
        return pair_->name_eq(base_, s, len);
    }
    bool name_eq(std::string_view str) const noexcept {
        return pair_->name_eq(base_, str);
    }
    bool name_eq_case(const char* s, size_t len) const noexcept {
        return pair_->name_eq_case(base_, s, len);
    }
    bool name_eq_case(std::string_view str) const noexcept {
        return pair_->name_eq_case(base_, str);
    }

    constexpr std::string_view value() const noexcept {
        return std::string_view{base_ + pair_->value1, base_ + pair_->value2};
    }

    value_type operator*() const noexcept { return {name(), value()}; }
    pointer operator->() const noexcept { return {operator*()}; }

    auto operator<=>(const http_header_iterator& x) const noexcept {
        return pair_ <=> x.pair_;
    }
    bool operator==(const http_header_iterator& x) const noexcept {
        return pair_ == x.pair_;
    }

    http_header_iterator& operator++() { ++ptr_; return *this; }
    http_header_iterator operator++(int) { auto tmp{*this}; ++pair_; return tmp; }

    http_header_iterator& operator--() { --ptr_; return *this; }
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

    inline http_message();

    inline bool ok() const;
    explicit inline operator bool() const;
    inline bool operator!() const;
    inline enum http_errno error() const;

    inline unsigned http_major() const;
    inline unsigned http_minor() const;

    inline unsigned status_code() const;
    inline const std::string& status_message() const;
    inline enum http_method method() const;
    inline const std::string& url() const;

    inline bool has_header(const char* name) const;
    inline bool has_header(const char* name, size_t length) const;
    inline bool has_header(std::string_view name) const;
    inline header_iterator find_header(const char* name) const;
    header_iterator find_header(const char* name, size_t length) const;
    inline header_iterator find_header(std::string_view name) const;
    inline std::string header(const char* name) const;
    std::string header(const char* name, size_t length) const;
    inline std::string header(std::string_view name) const;

    inline const std::string& body() const;

    std::string_view host() const;
    inline std::string_view url_schema() const;
    inline std::string_view url_host() const;
    std::string_view url_host_port() const;
    uint16_t url_port() const;
    inline std::string_view url_path() const;
    inline bool has_query() const;
    inline std::string_view query() const;
    bool has_query(std::string_view name) const;
    std::string_view query(std::string_view name) const;

    inline header_iterator header_begin() const;
    inline header_iterator header_end() const;
    inline header_iterator query_begin() const;
    inline header_iterator query_end() const;

    inline http_message& clear();
    void add_header(std::string key, std::string value);

    inline http_message& http_major(unsigned v);
    inline http_message& http_minor(unsigned v);
    inline http_message& error(enum http_errno e);
    inline http_message& status_code(unsigned code);
    inline http_message& status_code(unsigned code, std::string message);
    inline http_message& method(enum http_method method);
    inline http_message& url(std::string url);
    inline http_message& header(std::string key, std::string value);
    inline http_message& header(std::string key, size_t value);
    inline http_message& date_header(std::string key, time_t value);
    inline http_message& body(std::string body);
    inline http_message& append_body(const std::string& x);

    static const char* default_status_message(unsigned code);

  private:
    enum {
        info_url = 1, info_query = 2
    };

    struct info_type {
        unsigned flags;
        struct http_parser_url urlp;
        std::string qurl;
        std::vector<http_opair> qpairs;
        inline info_type()
            : flags(0) {
        }
    };

    unsigned short major_;
    unsigned short minor_;
    unsigned status_code_ : 16;
    unsigned method_ : 8;
    unsigned error_ : 7;
    unsigned upgrade_ : 1;

    std::string url_;
    std::string status_message_;
    std::string header_;
    std::vector<http_opair> hpairs_;
    std::string body_;

    mutable std::shared_ptr<info_type> info_;

    inline void kill_info(unsigned f) const;
    inline info_type& info(unsigned f) const;
    void make_info(unsigned f) const;
    inline bool has_url_field(int field) const;
    inline std::string url_field(int field) const;
    void do_clear();
    friend class http_parser;
};

class http_parser {
public:
    http_parser(fd f, enum llhttp_parser_type type);

    void clear();
    inline bool ok() const;
    inline enum llhttp_errno error() const;
    inline bool should_keep_alive() const;

    void receive(fd f, event<http_message> done);
    void send(fd f, const http_message& m, event<> done);
    static void send_request(fd f, const http_message& m, event<> done);
    static void send_request(fd f, http_message&& m, event<> done);
    static void send_response(fd f, const http_message& m, event<> done);
    static void send_response(fd f, http_message&& m, event<> done);
    static void send_response_headers(fd f, const http_message& m, event<> done);
    static void send_response_chunk(fd f, std::string s, event<> done);
    static void send_response_end(fd f, event<> done);

    inline void clear_should_keep_alive();

private:
    fd f_;
    ::llhttp_parser hp_;

    enum { state_unknown, state_header_name, state_header_value, state_done };

    struct message_data {
        http_message hm;
        int state = state_unknown;
        unsigned name1;
        unsigned name2;
        unsigned value1;
    };

    static const http_parser_settings settings;
    static http_parser* get_parser(::llhttp_parser* hp);
    static message_data* get_message_data(::llhttp_parser* hp);
    static int on_message_begin(::llhttp_parser* hp);
    static int on_url(::llhttp_parser* hp, const char* s, size_t len);
    static int on_status(::llhttp_parser* hp, const char* s, size_t len);
    static int on_header_field(::llhttp_parser* hp, const char* s, size_t len);
    static int on_header_field_complete(::llhttp_parser* hp);
    static int on_header_value(::llhttp_parser* hp, const char* s, size_t len);
    static int on_header_value_complete(::llhttp_parser* hp);
    static int on_headers_complete(::llhttp_parser* hp);
    static int on_body(::llhttp_parser* hp, const char* s, size_t len);
    static int on_message_complete(::llhttp_parser* hp);
    inline void copy_parser_status(message_data& md);
    static void unparse_request_headers(std::ostringstream& buf,
                                        const http_message& m);
    static void unparse_response_headers(std::ostringstream& buf,
                                         const http_message& m,
                                         bool include_content_length);
    static inline std::string prepare_headers(const http_message& m,
                                              std::string& body,
                                              bool is_response);
    static inline void send_message(fd f, std::string headers,
                                    std::string body, event<> done);
    static void send_two(fd f, std::string a, std::string b, event<> done);
};

inline http_message::http_message()
    : major_(1), minor_(1), status_code_(200), method_(HTTP_GET),
      error_(HPE_OK), upgrade_(0) {
}

inline void http_message::kill_info(unsigned f) const {
    if (info_)
        info_->flags &= ~f;
}

inline unsigned http_message::http_major() const {
    return major_;
}

inline unsigned http_message::http_minor() const {
    return minor_;
}

inline bool http_message::ok() const {
    return error_ == HPE_OK;
}

inline http_message::operator bool() const {
    return ok();
}

inline bool http_message::operator!() const {
    return !ok();
}

inline enum http_errno http_message::error() const {
    return (enum http_errno) error_;
}

inline unsigned http_message::status_code() const {
    return status_code_;
}

inline const std::string& http_message::status_message() const {
    return status_message_;
}

inline enum http_method http_message::method() const {
    return (enum http_method) method_;
}

inline const std::string& http_message::url() const {
    return url_;
}

inline bool http_message::has_header(const char* name, size_t length) const {
    return find_header(name, length) != header_end();
}

inline bool http_message::has_header(const char* name) const {
    return find_header(name) != header_end();
}

inline bool http_message::has_header(std::string_view name) const {
    return find_header(name) != header_end();
}

inline http_message::header_iterator http_message::find_header(const char* name) const {
    return find_header(name, strlen(name));
}

inline http_message::header_iterator http_message::find_header(std::string_view name) const {
    return find_header(name.data(), name.length());
}

inline std::string_view http_message::header(const char* name) const {
    return header(name, strlen(name));
}

inline std::string_view http_message::header(std::string_view name) const {
    return header(name.data(), name.length());
}

inline const std::string& http_message::body() const {
    return body_;
}

inline bool http_message::has_url_field(int field) const {
    return info(info_url).urlp.field_set & (1 << field);
}

inline std::string_view http_message::url_field(int field) const {
    const info_type& i = info(info_url);
    if (i.urlp.field_set & (1 << field)) {
        return std::string_view(url_.data() + i.urlp.field_data[field].off,
                                i.urlp.field_data[field].len);
    }
    return std::string_view();
}

inline bool http_message::has_query() const {
    return has_url_field(UF_QUERY);
}

inline std::string_view http_message::query() const {
    return url_field(UF_QUERY);
}

inline std::string_view http_message::url_schema() const {
    return url_field(UF_SCHEMA);
}

inline std::string_view http_message::url_host() const {
    return url_field(UF_HOST);
}

inline std::string_view http_message::url_path() const {
    return url_field(UF_PATH);
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

inline http_message& http_message::error(enum http_errno e) {
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

inline http_message& http_message::method(enum http_method method) {
    method_ = (unsigned) method;
    return *this;
}

inline http_message& http_message::url(std::string url) {
    url_ = std::move(url);
    kill_info(info_url | info_query);
    return *this;
}

inline http_message& http_message::header(std::string key, std::string value) {
    add_header(std::move(key), std::move(value));
    return *this;
}

inline http_message& http_message::header(std::string key, size_t value) {
    add_header(std::move(key), std::to_s(value));
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
    return *this;
}

inline http_message& http_message::append_body(const std::string& x) {
    body_ += x;
    return *this;
}

inline http_message::info_type& http_message::info(unsigned f) const {
    if (!info_ || !info_.unique() || (info_->flags & f) != f) {
        make_info(f);
    }
    return *info_.get();
}

inline http_message::header_iterator http_message::header_begin() const {
    return http_header_iterator{headers_.data(), hpairs_.begin()};
}

inline http_message::header_iterator http_message::header_end() const {
    return http_header_iterator{headers_.data(), hpairs_.end()};
}

inline http_message::header_iterator http_message::query_begin() const {
    auto& i = info(info_query);
    return http_header_iterator{i.qurl.data(), i.qpairs.begin()};
}

inline http_message::header_iterator http_message::query_end() const {
    auto& i = info(info_query);
    return http_header_iterator{i.qurl.data(), i.qpairs.end()};
}

inline bool http_parser::ok() const {
    return hp_.http_errno == (unsigned) HPE_OK;
}

inline enum http_errno http_parser::error() const {
    return (enum http_errno) hp_.http_errno;
}

inline bool http_parser::should_keep_alive() const {
    return llhttp_should_keep_alive(&hp_);
}

inline void http_parser::clear_should_keep_alive() {
    hp_.flags = (hp_.flags & ~F_CONNECTION_KEEP_ALIVE) | F_CONNECTION_CLOSE;
}

inline void http_parser::send_message(fd f, std::string headers,
                                      std::string body, event<> done) {
    if (body.empty()) {
        f.write(headers, done);
    } else {
        send_two(f, headers, body, done);
    }
}

} // namespace cotamer
