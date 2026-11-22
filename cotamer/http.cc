#include "cotamer/http.hh"
#include "cotamer/http_fields.hh"
#include <cstring>
#if COTAMER_HAVE_NLOHMANN_JSON
# include <nlohmann/json.hpp>
#endif

namespace {
struct status_code_map {
    unsigned code;
    std::string_view message;
} default_status_codes[] = {
    {100, "Continue"},
    {101, "Switching Protocols"},
    {102, "Processing"},                // RFC 2518, obsoleted by RFC 4918
    {103, "Early Hints"},               // RFC 8297
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {207, "Multi-Status"},              // RFC 4918
    {208, "Already Reported"},          // RFC 5842
    {226, "IM Used"},                   // RFC 3229
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Content Too Large"},
    {414, "URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {418, "I'm a teapot"},              // RFC 2324
    {421, "Misdirected Request"},
    {422, "Unprocessable Content"},     // RFC 4918
    {423, "Locked"},                    // RFC 4918
    {424, "Failed Dependency"},         // RFC 4918
    {425, "Too Early"},                 // RFC 8470
    {426, "Upgrade Required"},          // RFC 2817
    {428, "Precondition Required"},     // RFC 6585
    {429, "Too Many Requests"},         // RFC 6585
    {431, "Request Header Fields Too Large"}, // RFC 6585
    {451, "Unavailable For Legal Reasons"}, // RFC 7725
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {506, "Variant Also Negotiates"},   // RFC 2295
    {507, "Insufficient Storage"},      // RFC 4918
    {508, "Loop Detected"},
    {509, "Bandwidth Limit Exceeded"},
    {510, "Not Extended"},              // RFC 2774
    {511, "Network Authentication Required"} // RFC 6585
};
struct status_code_map_comparator {
    bool operator()(const status_code_map& a, unsigned b) {
        return a.code < b;
    }
};

using cotamer::detail::chartable;
using cotamer::detail::us_http_tchar;
using cotamer::detail::us_hex;
using cotamer::detail::us_header;
using cotamer::detail::us_target;
using cotamer::detail::us_ows;

// Bodies reserve space for at most this many bytes in advance
constexpr size_t max_body_reserve = 1 << 20;

// `ch` must be a hex digit
inline int xvalue(unsigned char ch) {
    return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
}

inline bool all_chars(std::string_view s, uint16_t mask) {
    for (unsigned char ch : s) {
        if (!(chartable.v[ch] & mask)) {
            return false;
        }
    }
    return true;
}

// Request targets additionally exclude whitespace, which delimits the
// request line.
inline bool valid_request_target(std::string_view s) {
    return all_chars(s, us_target);
}

int parse_method_on_method_complete(llhttp_t*) {
    return -1;
}

const llhttp_settings_t parse_method_settings = []{
    llhttp_settings_t s{};
    s.on_method_complete = parse_method_on_method_complete;
    return s;
}();

}

namespace cotamer {

const llhttp_settings_t http_parser::settings = []{
    llhttp_settings_t s{};
    s.on_message_begin = &http_parser::on_message_begin;
    s.on_url = &http_parser::on_url;
    s.on_status = &http_parser::on_status;
    s.on_header_field = &http_parser::on_header_field;
    s.on_header_value = &http_parser::on_header_value;
    s.on_headers_complete = &http_parser::on_headers_complete;
    s.on_body = &http_parser::on_body;
    s.on_message_complete = &http_parser::on_message_complete;
    s.on_header_field_complete = &http_parser::on_header_field_complete;
    s.on_header_value_complete = &http_parser::on_header_value_complete;
    s.on_chunk_header = &http_parser::on_chunk_header;
    return s;
}();

std::string_view http_message::default_status_message(unsigned code) {
    if (code == 200) { // common case
        return "OK";
    }
    size_t ncodes = sizeof(default_status_codes) / sizeof(status_code_map);
    status_code_map* m = std::lower_bound(default_status_codes,
                                          default_status_codes + ncodes,
                                          code,
                                          status_code_map_comparator());
    if (m != default_status_codes + ncodes && m->code == code) {
        return m->message;
    }
    return "";
}


bool http_message::has_header(std::string_view name) const {
    auto it = header_begin(), last = header_end();
    while (it != last) {
        if (it.name_ieq(name)) {
            return true;
        }
        ++it;
    }
    return false;
}

http_message::header_iterator http_message::find_header(std::string_view name) const {
    auto it = header_begin(), last = header_end();
    while (it != last && !it.name_ieq(name)) {
        ++it;
    }
    return it;
}

std::string http_message::header(std::string_view name) const {
    std::string result;
    bool any = false;
    auto it = header_begin(), last = header_end();
    while (it != last) {
        if (it.name_ieq(name)) {
            if (any) {
                result += ", ";
                result += it.value();
            } else {
                result = it.value();
                any = true;
            }
        }
        ++it;
    }
    return result;
}

void http_message::do_clear() {
    major_ = minor_ = 1;
    status_code_ = 200;
    method_ = HTTP_GET;
    error_ = HPE_OK;
    upgrade_ = has_body_ = headers_sparse_ = 0;
    url_.clear();
    status_message_.clear();
    headers_.clear();
    hpairs_.clear();
    body_.clear();
    if (info_) {
        info_->flags = 0;
    }
}

http_message& http_message::add_header(std::string_view key, std::string_view value) {
    if (key.empty() || !all_chars(key, us_http_tchar)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!all_chars(value, us_header)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    if (headers_.empty()) {
        headers_.reserve(2048);
    }
    size_t name1 = headers_.length(), name2 = name1 + key.length();
    headers_ += key;
    headers_.append(": ", 2);
    headers_ += strings::trim_hws(value);
    headers_.append("\r\n", 2);
    hpairs_.emplace_back(name1, name2, name2 + 2, headers_.length() - 2);
    return *this;
}

// Replace framing headers with a Content-Length that matches the body.
void http_message::set_framing() {
    remove_header_if([](std::string_view name, std::string_view) {
        return strings::ieq(name, "content-length")
            || strings::ieq(name, "transfer-encoding");
    });
    add_header("Content-Length", body_.length());
}

http_message& http_message::remove_header(std::string_view key) {
    return remove_header_if([key](std::string_view name, std::string_view) {
        return strings::ieq(name, key);
    });
}

// Drop removed headers' lines from `headers_`. Each line is
// `name: value\r\n`. Runs of adjacent lines move together.
void http_message::compact_headers() {
    char* s = headers_.data();
    size_t i = 0, n = hpairs_.size();
    size_t d = n ? hpairs_[0].name1 : 0;
    while (i != n) {
        size_t p1 = hpairs_[i].name1, p2 = hpairs_[i].value2 + 2;
        size_t j = i + 1;
        while (j != n && hpairs_[j].name1 == p2) {
            p2 = hpairs_[j].value2 + 2;
            ++j;
        }
        if (d) {
            std::memmove(s + p1 - d, s + p1, p2 - p1);
            while (i != j) {
                hpairs_[i].name1 -= d;
                hpairs_[i].name2 -= d;
                hpairs_[i].value1 -= d;
                hpairs_[i].value2 -= d;
                ++i;
            }
        }
        if (j != n) {
            d += hpairs_[j].name1 - p2;
        }
        i = j;
    }
    headers_.resize(n ? hpairs_[n - 1].value2 + 2 : 0);
    headers_sparse_ = 0;
}

void http_message::make_info(unsigned fl) const {
    if (!info_) {
        info_ = std::make_unique<info_type>();
    }
    info_type& inf = *info_;

    if (!(inf.flags & info_url) && (fl & (info_url | info_params))) {
        // origin-form or absolute-form URL
        strings::url_view u(url_);
        using uv = strings::url_view;
        inf.f[0] = {u.p_[uv::p_path], u.p_[uv::p_search]};
        inf.f[1] = {u.p_[uv::p_search], u.p_[uv::p_hash]};
        inf.f[2] = {u.p_[uv::p_hash], u.p_[uv::p_end]};
        inf.flags = (inf.flags & ~info_valid_url) | info_url
            | (u.ok() ? info_valid_url : 0);
    }

    if (!(inf.flags & info_params) && (fl & info_params)) {
        inf.qurl.clear();
        inf.qpairs.clear();
        if (inf.f[1].first != inf.f[1].second) {
            const char* s1 = url_.data() + inf.f[1].first + 1;
            const char* s2 = url_.data() + inf.f[1].second;
            inf.qurl.reserve(s2 - s1);
            int state = 0; // 0: beginning, 1: in name, 2: in value
            const char* s = s1;
            const char* last = s1;
            unsigned nstart = 0, nend = 0, vstart = 0, vend = 0;
            while (s != s2) {
                if (*s == '&') {
                    if (state != 0) {
                        vend = inf.qurl.length() + s - last;
                        if (state == 1) {
                            nend = vstart = vend;
                        }
                        inf.qpairs.emplace_back(nstart, nend, vstart, vend);
                        state = 0;
                    }
                    ++s;
                    nstart = nend = inf.qurl.length() + s - last;
                    continue;
                }
                if (*s == '=' && state <= 1) {
                    nend = inf.qurl.length() + s - last;
                    vstart = nend + 1;
                    state = 2;
                    ++s;
                    continue;
                }
                if (state == 0) {
                    state = 1;
                }
                if (*s == '%'
                    && s2 - s >= 3
                    && (chartable.v[(unsigned char) s[1]] & us_hex)
                    && (chartable.v[(unsigned char) s[2]] & us_hex)) {
                    inf.qurl.append(last, s);
                    inf.qurl.push_back(xvalue(s[1]) * 16 + xvalue(s[2]));
                    last = s = s + 3;
                } else if (*s == '+') {
                    inf.qurl.append(last, s);
                    inf.qurl.push_back(' ');
                    last = s = s + 1;
                } else {
                    ++s;
                }
            }
            if (last != s) {
                inf.qurl.append(last, s);
            }
            if (state != 0) {
                vend = inf.qurl.length();
                if (state == 1) {
                    nend = vstart = vend;
                }
                inf.qpairs.emplace_back(nstart, nend, vstart, vend);
            }
        }
        inf.flags |= info_params;
    }
}

std::optional<std::string> http_message::decoded_path() const {
    if (!has_valid_url()) {
        return std::nullopt;
    }
    // `path()` starts with `/`; examine the segments that follow each `/`
    std::string_view p = path();
    std::vector<std::string> segments;
    size_t pos = 1;
    while (true) {
        size_t slash = std::min(p.find('/', pos), p.size());
        std::string seg;
        for (size_t i = pos; i != slash; ++i) {
            unsigned char ch = p[i];
            if (ch == '%') {
                if (slash - i < 3
                    || !(chartable.v[(unsigned char) p[i + 1]] & us_hex)
                    || !(chartable.v[(unsigned char) p[i + 2]] & us_hex)) {
                    return std::nullopt;
                }
                ch = xvalue(p[i + 1]) * 16 + xvalue(p[i + 2]);
                if (ch == '\0' || ch == '/') {
                    return std::nullopt;
                }
                i += 2;
            }
            seg.push_back(ch);
        }
        bool last = slash == p.size();
        if (seg == "." || seg == "..") {
            if (seg == "..") {
                if (segments.empty()) {
                    return std::nullopt;
                }
                segments.pop_back();
            }
            // a final dot segment leaves a trailing slash
            if (last) {
                segments.emplace_back();
            }
        } else {
            segments.push_back(std::move(seg));
        }
        if (last) {
            break;
        }
        pos = slash + 1;
    }
    std::string result;
    for (auto& seg : segments) {
        result.push_back('/');
        result += seg;
    }
    return result;
}

bool http_message::has_search_param(std::string_view name) const {
    const info_type& inf = info(info_params);
    auto it = inf.qpairs.begin(), end = inf.qpairs.end();
    while (it != end) {
        if (it->name(inf.qurl.data()) == name) {
            return true;
        }
        ++it;
    }
    return false;
}

std::string_view http_message::search_param(std::string_view name) const {
    const info_type& inf = info(info_params);
    auto it = inf.qpairs.begin(), end = inf.qpairs.end();
    while (it != end) {
        if (it->name(inf.qurl.data()) == name) {
            return it->value(inf.qurl.data());
        }
        ++it;
    }
    return std::string_view{};
}


llhttp_method http_parser::parse_method(std::string_view s) {
    llhttp_t hp;
    llhttp_init(&hp, HTTP_REQUEST, &parse_method_settings);
    llhttp_errno_t err = llhttp_execute(&hp, s.data(), s.size());
    const char space[] = " ";
    if (err == HPE_OK) {
        err = llhttp_execute(&hp, space, 1);
    }
    if (err == HPE_CB_METHOD_COMPLETE
        && (hp.error_pos == s.data() + s.size() || hp.error_pos == space)) {
        return (llhttp_method) hp.method;
    }
    throw std::invalid_argument("unknown HTTP method");
}


http_parser::http_parser(fd f, parser_type direction, std::string host)
    : http_parser(std::move(f), direction == client ? HTTP_RESPONSE : HTTP_REQUEST) {
    host_ = std::move(host);
}

http_parser::http_parser(fd f, enum llhttp_type receive_type)
    : f_(std::move(f)),
      receive_timeout_(receive_type == HTTP_REQUEST
                       ? default_server_receive_timeout
                       : duration::max()) {
    llhttp_init(&hp_, receive_type, &settings);
}

void http_parser::clear() {
    llhttp_reset(&hp_);
    receive_buffer_.clear();
    failure_ = failure_type::none;
    upgraded_ = false;
    eof_ = false;
    pending_methods_.clear();
}

void http_parser::fail(message_data& md, failure_type f) {
    static const char* const reasons[] = {
        "", "header too large", "body too large",
        "receive timeout", "receive cancelled"
    };
    failure_ = f;
    hp_.error = HPE_USER;
    llhttp_set_error_reason(&hp_, reasons[int(f)]);
    md.hm.error_ = HPE_USER;
}

// Record a failure in a callback; `receive()` reports it after
// `llhttp_execute()` returns.
int http_parser::fail_callback(::llhttp_t* hp, failure_type f) {
    get_message_data(hp)->failure = f;
    return HPE_USER;
}

// Return true if `len` more bytes of request line, status line, or headers
// fit within `max_header_size()`.
bool http_parser::check_header_size(::llhttp_t* hp, size_t len) {
    const http_message& m = get_message_data(hp)->hm;
    size_t max = get_parser(hp)->max_header_size_;
    size_t sz = m.url_.size() + m.status_message_.size() + m.headers_.size();
    return sz <= max && len <= max - sz;
}

inline void http_parser::copy_parser_status(message_data& md) {
    md.hm.major_ = hp_.http_major;
    md.hm.minor_ = hp_.http_minor;
    md.hm.status_code_ = hp_.status_code;
    // a response reports the method of its request
    if (hp_.type == HTTP_REQUEST) {
        md.hm.method_ = hp_.method;
    } else if (!pending_methods_.empty()) {
        md.hm.method_ = pending_methods_.front();
    }
    md.hm.error_ = hp_.error;
    md.hm.upgrade_ = hp_.upgrade;
}

inline http_parser* http_parser::get_parser(::llhttp_t* hp) {
    return reinterpret_cast<http_parser*>(hp);
}

inline http_parser::message_data* http_parser::get_message_data(::llhttp_t* hp) {
    return static_cast<message_data*>(hp->data);
}

int http_parser::on_message_begin(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    md->hm.clear();
    md->hm.status_code_ = 0;
    md->state = state_unknown;
    md->begun = true;
    return 0;
}

int http_parser::on_url(::llhttp_t* hp, const char* s, size_t len) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, len)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    md->hm.url_.append(s, len);
    md->state = state_unknown;
    return 0;
}

int http_parser::on_status(::llhttp_t* hp, const char* s, size_t len) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, len)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    md->hm.status_message_.append(s, len);
    md->state = state_unknown;
    return 0;
}

int http_parser::on_header_field(::llhttp_t* hp, const char* s, size_t len) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, len)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    if (md->state != state_header_name) {
        md->name1 = md->hm.headers_.length();
        md->state = state_header_name;
    }
    md->hm.headers_.append(s, len);
    return 0;
}

int http_parser::on_header_field_complete(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, 2)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    if (md->state != state_header_name) {
        md->name1 = md->hm.headers_.length();
    }
    md->name2 = md->hm.headers_.length();
    md->hm.headers_.append(": ", 2);
    md->value1 = md->name2 + 2;
    md->state = state_header_value;
    return 0;
}

int http_parser::on_header_value(::llhttp_t* hp, const char* s, size_t len) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, len)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    md->hm.headers_.append(s, len);
    return 0;
}

int http_parser::on_header_value_complete(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    if (!check_header_size(hp, 2)) {
        return fail_callback(hp, failure_type::header_too_large);
    }
    if (md->state == state_header_value) {
        // strip trailing OWS (llhttp stripped leading OWS)
        const char* s = md->hm.headers_.data();
        size_t v2 = md->hm.headers_.length();
        while (v2 > md->value1 && (chartable.v[(unsigned char) s[v2 - 1]] & us_ows)) {
            --v2;
        }
        md->hm.headers_.resize(v2);
        md->hm.hpairs_.emplace_back(md->name1, md->name2, md->value1, v2);
    }
    md->hm.headers_.append("\r\n", 2);
    md->state = state_unknown;
    return 0;
}

// Return true if `len` more body bytes fit within `max`.
static bool body_fits(const std::string& body, size_t max, uint64_t len) {
    return body.size() <= max && len <= max - body.size();
}

// Reserve space for `len` more body bytes, up to `max_body_reserve`.
static void reserve_body(std::string& body, uint64_t len) {
    size_t want = body.size() + std::min<uint64_t>(len, max_body_reserve);
    if (body.capacity() < want) {
        body.reserve(std::max(want, body.capacity() * 2));
    }
}

int http_parser::on_headers_complete(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    http_parser* p = get_parser(hp);
    p->copy_parser_status(*md);
    // A client knows from its request whether a final response has a body:
    // responses to HEAD do not, and a 2xx response to CONNECT starts a tunnel.
    if (hp->type == HTTP_RESPONSE
        && hp->status_code >= 200
        && !p->pending_methods_.empty()) {
        llhttp_method method = p->pending_methods_.front();
        if (method == HTTP_HEAD) {
            return 1;   // no body
        } else if (method == HTTP_CONNECT && hp->status_code < 300) {
            return 2;   // no body, upgrade
        }
    }
    if (!body_fits(md->hm.body_, get_parser(hp)->max_body_size_, hp->content_length)) {
        return fail_callback(hp, failure_type::body_too_large);
    }
    reserve_body(md->hm.body_, hp->content_length);
    return 0;
}

int http_parser::on_chunk_header(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    md->hm.has_body_ = 1;
    if (!body_fits(md->hm.body_, get_parser(hp)->max_body_size_, hp->content_length)) {
        return fail_callback(hp, failure_type::body_too_large);
    }
    reserve_body(md->hm.body_, hp->content_length);
    return 0;
}

int http_parser::on_body(::llhttp_t* hp, const char* s, size_t len) {
    message_data* md = get_message_data(hp);
    if (!body_fits(md->hm.body_, get_parser(hp)->max_body_size_, len)) {
        return fail_callback(hp, failure_type::body_too_large);
    }
    md->hm.body_.append(s, len);
    md->hm.has_body_ = 1;
    return 0;
}

int http_parser::on_message_complete(::llhttp_t* hp) {
    message_data* md = get_message_data(hp);
    md->state = state_done;
    // Pause llhttp so receive() task can yield
    return HPE_PAUSED;
}

// Detaches a `receive()` call's message data from the parser. If `receive()`
// is destroyed partway through a message (because it was cancelled or a
// callback threw), the parser cannot resume, so it is marked as failed.
struct http_parser::receive_guard {
    http_parser* hp;
    message_data* md;
    bool done = false;

    ~receive_guard() {
        if (!done && md->begun) {
            hp->fail(*md, failure_type::cancelled);
        }
        hp->hp_.data = nullptr;
    }
};

task<http_message> http_parser::receive(ticket_type ticket) {
    assert(ticket.lock_.mutex() == &m_[0]);
    char stackbuf[bufsize];
    message_data md;
    md.hm.status_code_ = 0;
    co_await std::move(ticket.ev_);
    unique_lock guard(std::move(ticket.lock_));

    // After an error, upgrade, or EOF, no more HTTP messages can be received
    if (upgraded_ && hp_.error == HPE_OK) {
        hp_.error = HPE_PAUSED_UPGRADE;
    } else if (eof_ && hp_.error == HPE_OK) {
        hp_.error = HPE_CLOSED_CONNECTION;
    }
    if (hp_.error != HPE_OK) {
        md.hm.error_ = hp_.error;
        co_return std::move(md.hm);
    }

    receive_guard rguard{this, &md};
    hp_.data = &md;
    bool timed = receive_timeout_ != duration::max();
    event deadline;
    if (timed) {
        deadline = after(receive_timeout_);
    }

    while (true) {
        const char* buf;
        size_t nr;
        if (receive_buffer_.empty()) {
            buf = stackbuf;
            ioresult nx;
            if (timed) {
                auto ax = co_await attempt(recv(f_, stackbuf, sizeof(stackbuf)), deadline);
                if (!ax) {
                    fail(md, failure_type::timeout);
                    break;
                }
                nx = *ax;
            } else {
                nx = co_await recv(f_, stackbuf, sizeof(stackbuf));
            }
            if (nx && *nx == 0) {
                // EOF can complete a message (e.g., a response without
                // Content-Length)
                eof_ = true;
                llhttp_finish(&hp_);
                if (hp_.error == HPE_PAUSED) {
                    llhttp_resume(&hp_);
                }
                if (md.state == state_done) {
                    copy_parser_status(md);
                }
            }
            if (!nx || *nx == 0) {
                break;
            }
            nr = *nx;
        } else {
            // Drain bytes left from the previous call (pipelined data, or a
            // partial frame we couldn't process in one llhttp_execute).
            buf = receive_buffer_.data();
            nr = receive_buffer_.size();
        }

        llhttp_errno_t err = llhttp_execute(&hp_, buf, nr);

        // save unconsumed bytes in `receive_buffer_`
        size_t consumed = nr;
        if (err != HPE_OK) {
            consumed = llhttp_get_error_pos(&hp_) - buf;
        }
        if (consumed == nr) {
            receive_buffer_.clear();
        } else {
            receive_buffer_ = std::string(buf + consumed, buf + nr);
        }

        if (err == HPE_PAUSED) {
            // we only pause at a message boundary
            llhttp_resume(&hp_);
            md.state = state_done;
        } else if (err != HPE_OK && md.failure != failure_type::none) {
            fail(md, md.failure);
        }
        if (err != HPE_OK || md.state == state_done) {
            copy_parser_status(md);
            break;
        }
        // otherwise, message incomplete: loop to read more.
    }

    if (md.state == state_done) {
        // Leave bytes following an upgrade in `receive_buffer_`
        if (md.hm.upgrade_) {
            upgraded_ = true;
        }
        if (hp_.type == HTTP_REQUEST) {
            pending_methods_.push_back(md.hm.method());
        } else if (md.hm.status_code_ >= 200 && !pending_methods_.empty()) {
            pending_methods_.pop_front();
        }
    } else if (!md.hm.error_) {
        hp_.error = md.hm.error_ = HPE_CLOSED_CONNECTION;
    }
    rguard.done = true;
    co_return std::move(md.hm);
}

task<http_parser::ticket_type> http_parser::send_request(http_message m) {
    if (!valid_request_target(m.url())) {
        throw std::invalid_argument("invalid HTTP request target");
    }
    std::string urlline = std::format("{} {} HTTP/{}.{}\r\n",
        m.method_name(), m.url(), m.http_major(), m.http_minor());
    if (!host_.empty()
        && !m.has_header("host")) {
        m.add_header("Host", host_);
    }
    if (m.has_body_) {
        m.set_framing();
    }
    if (m.headers_sparse_) {
        m.compact_headers();
    }
    m.headers_.append("\r\n", 2);

    iovec iov[3];
    iov[0] = iovec{ urlline.data(), urlline.length() };
    iov[1] = iovec{ m.headers_.data(), m.headers_.length() };
    size_t iovcnt = 2;
    if (m.body_.length()) {
        iov[2] = iovec{ m.body_.data(), m.body_.length() };
        ++iovcnt;
    }

    // lock for writing, then obtain read ticket; responses arrive in the
    // order requests are sent
    unique_lock guard(co_await m_[1].lock());
    pending_methods_.push_back(m.method());
    ticket_type ticket(m_[0]);

    co_await sendv_all(f_, iov, iovcnt);
    co_return std::move(ticket);
}

task<> http_parser::send_response(http_message m) {
    unsigned code = m.status_code();
    if (code < 100 || code > 999) {
        throw std::invalid_argument("invalid HTTP status code");
    }
    std::string_view status_message = m.status_message_.empty()
        ? m.default_status_message(code)
        : std::string_view(m.status_message());
    if (!all_chars(status_message, us_header)) {
        throw std::invalid_argument("invalid HTTP status message");
    }

    // Lock before matching this response with its request
    unique_lock guard(co_await m_[1].lock());
    bool head = false;
    if (code >= 200 && !pending_methods_.empty()) {
        head = pending_methods_.front() == HTTP_HEAD;
        pending_methods_.pop_front();
    }

    // If the response is marked `Connection: close`, then ensure
    // should_keep_alive() returns 0
    if (should_keep_alive()) {
        std::string connhdr = m.header("connection");
        if (strings::ieq(connhdr, "close")) {
            if (hp_.http_major > 0 && hp_.http_minor > 0) {
                hp_.flags |= F_CONNECTION_CLOSE;
            } else {
                hp_.flags &= ~F_CONNECTION_KEEP_ALIVE;
            }
        }
    }

    std::string codeline = std::format("HTTP/{}.{} {} {}\r\n",
        m.http_major(), m.http_minor(), code, status_message);

    // 1xx, 204, and 304 responses never have a body. A response with a body
    // gets a correct Content-Length, replacing any framing headers; other
    // responses get `Content-Length: 0` unless they specify their own
    // framing. A response to HEAD omits the body, but its Content-Length
    // reports the body's length (if the body is nonempty).
    bool bodyless = code < 200 || code == 204 || code == 304;
    if (!bodyless && !(head && m.body_.empty())) {
        if (m.has_body_) {
            m.set_framing();
        } else if (!m.has_header("content-length")
                   && !m.has_header("transfer-encoding")) {
            m.add_header("Content-Length", m.body_.length());
        }
    }
    if (m.headers_sparse_) {
        m.compact_headers();
    }
    m.headers_.append("\r\n", 2);

    iovec iov[3];
    iov[0] = iovec{ codeline.data(), codeline.length() };
    iov[1] = iovec{ m.headers_.data(), m.headers_.length() };
    size_t iovcnt = 2;
    if (!bodyless && !head && m.body_.length()) {
        iov[2] = iovec{ m.body_.data(), m.body_.length() };
        ++iovcnt;
    }

    co_await sendv_all(f_, iov, iovcnt);
}

task<> http_parser::send(http_message m) {
    assert(hp_.type == (int) HTTP_RESPONSE || hp_.type == (int) HTTP_REQUEST);
    if (hp_.type == (int) HTTP_RESPONSE) {
        co_await send_request(std::move(m));
    } else {
        co_await send_response(std::move(m));
    }
}

// An empty chunk would terminate the body, so it is ignored; use
// `send_response_end_chunk()` to terminate the body.
task<> http_parser::send_response_chunk(std::string str) {
    if (str.empty()) {
        co_return;
    }
    std::string lenline = std::format("{:X}\r\n", str.length());
    iovec iov[3];
    iov[0] = iovec{ lenline.data(), lenline.length() };
    iov[1] = iovec{ str.data(), str.length() };
    iov[2] = iovec{ (void*) "\r\n", 2 };
    unique_lock guard(co_await m_[1].lock());
    co_await sendv_all(f_, iov, 3);
}

task<> http_parser::send_response_end_chunk() {
    unique_lock guard(co_await m_[1].lock());
    co_await send_all(f_, "0\r\n\r\n", 5);
}

} // namespace cotamer
