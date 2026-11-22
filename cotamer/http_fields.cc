#include "cotamer/http_fields.hh"
#include <algorithm>

namespace cotamer::detail {

constinit const charinfo chartable = []{
    charinfo ct{};
    for (unsigned char ch = '0'; ch <= '9'; ++ch) {
        ct.v[ch] = us_safe | us_http_tchar | us_hex | us_scheme;
    }
    for (unsigned char ch = 'A'; ch <= 'Z'; ++ch) {
        ct.v[ch] = ct.v[ch + 32] = us_safe | us_http_tchar | us_alpha | us_scheme
            | (ch <= 'F' ? us_hex : 0);
    }
    for (auto s = "!#$%&'*+-.^_`|~"; *s; ++s) {
        ct.v[(unsigned char) *s] |= us_http_tchar;
    }
    for (auto s = "!$%&'()*+,-./:;=@[\\]^_`{|}~"; *s; ++s) {
        ct.v[(unsigned char) *s] |= us_safe;
    }
    for (auto s = "+-."; *s; ++s) {
        ct.v[(unsigned char) *s] |= us_scheme;
    }
    for (unsigned ch = 0; ch != 256; ++ch) {
        if (ct.v[ch] & us_safe) {
            ct.v[ch] |= us_query | us_fragment;
        }
    }
    ct.v[(unsigned char) '?'] |= us_query | us_fragment;
    ct.v[(unsigned char) '#'] |= us_fragment;
    for (unsigned char ch = 1; ch != 0; ++ch) {
        if (ch != '\r' && ch != '\n') {
            ct.v[ch] |= us_header;
            if (ch != ' ' && ch != '\t') {
                ct.v[ch] |= us_target;
            }
        }
    }
    ct.v[(unsigned char) ' '] |= us_ows;
    ct.v[(unsigned char) '\t'] |= us_ows;
    return ct;
}();

} // namespace cotamer::detail


namespace cotamer::strings {

namespace {
inline uint16_t cclass(char ch) {
    return detail::chartable.v[(unsigned char) ch];
}
}

auto http_value_list::iterator::operator++() noexcept -> iterator& {
    const char* s = rest_.data();
    const char* last = s + rest_.size();

    while (s != last && ((cclass(*s) & detail::us_ows) || *s == ',')) {
        ++s;
    }
    if (s == last) {
        rest_.remove_prefix(rest_.size());
        value_ = std::string_view{s, 0};
        return *this;
    }

    const char* first = s;
    const char* last_nws = s;
    while (s != last) {
        if (cclass(*s) & detail::us_ows) {
            ++s;
        } else if (*s == ',') {
            break;
        } else if (*s == '\"') {
            for (++s; s != last; ++s) {
                if (*s == '\\' && s + 1 != last) {
                    ++s;
                } else if (*s == '\"') {
                    ++s;
                    break;
                }
            }
            last_nws = s;
        } else {
            ++s;
            last_nws = s;
        }
    }

    rest_.remove_prefix(s - rest_.data());
    value_ = std::string_view{first, last_nws};
    return *this;
}

auto http_parameter_list::iterator::operator++() noexcept -> iterator& {
    const char* s = rest_.data();
    const char* last = s + rest_.size();

    while (s != last && ((cclass(*s) & detail::us_ows) || *s == ';')) {
        ++s;
    }
    if (s == last) {
        rest_.remove_prefix(rest_.size());
        span_ = std::string_view{s, 0};
        namelen_ = 0;
        return *this;
    }

    const char* first = s;
    const char* last_name = s;
    const char* last_nws = s;
    while (s != last) {
        if (cclass(*s) & detail::us_ows) {
            ++s;
        } else if (*s == ';') {
            break;
        } else if (*s == '\"') {
            for (++s; s != last; ++s) {
                if (*s == '\\' && s + 1 != last) {
                    ++s;
                } else if (*s == '\"') {
                    ++s;
                    break;
                }
            }
            last_nws = s;
        } else if (last_name == s && (cclass(*s) & detail::us_http_tchar)) {
            ++s;
            last_nws = s;
            last_name = s;
        } else {
            ++s;
            last_nws = s;
        }
    }

    rest_.remove_prefix(s - rest_.data());
    span_ = std::string_view{first, last_nws};
    if (last_name > first
        && last_name < last_nws
        && *last_name == '='
        && (last_name + 1 == last_nws
            || (last_name[1] != ' ' && last_name[1] != '\t'))) {
        namelen_ = last_name - first;
    } else {
        namelen_ = span_.size();
    }
    return *this;
}

url_view::url_view(std::string_view url) noexcept
    : s_(url.data()) {
    if (!parse(url)) {
        std::fill(std::begin(p_), std::end(p_), 0);
    }
}

// Set the part boundaries; return false if `url` is invalid.
bool url_view::parse(std::string_view url) noexcept {
    using namespace detail;
    size_t pos = 0, n = url.size();
    if (n > UINT32_MAX) {
        return false;
    }

    if (n != 0 && url[0] == '/') {
        // origin-form: no scheme or authority
        p_[p_scheme_end] = p_[p_authority] = p_[p_host_port] = 0;
        p_[p_host_end] = p_[p_port] = 0;
    } else {
        // scheme://authority
        if (n == 0 || !(cclass(url[0]) & us_alpha)) {
            return false;
        }
        while (pos != n && (cclass(url[pos]) & us_scheme)) {
            ++pos;
        }
        if (url.substr(pos, 3) != "://") {
            return false;
        }
        p_[p_scheme_end] = pos;
        pos += 3;
        p_[p_authority] = p_[p_host_port] = pos;
        while (pos != n && (cclass(url[pos]) & us_safe) && url[pos] != '/') {
            ++pos;
        }
        std::string_view authority = url.substr(p_[p_authority], pos - p_[p_authority]);
        if (size_t at = authority.rfind('@'); at != std::string_view::npos) {
            p_[p_host_port] += at + 1;
        }
        std::string_view host_port = url.substr(p_[p_host_port], pos - p_[p_host_port]);
        size_t colon;
        if (!host_port.empty() && host_port[0] == '[') {
            size_t close = host_port.find(']');
            if (close == std::string_view::npos
                || (close + 1 != host_port.size() && host_port[close + 1] != ':')) {
                return false;
            }
            colon = close + 1;
        } else {
            colon = std::min(host_port.find(':'), host_port.size());
        }
        p_[p_host_end] = p_[p_host_port] + colon;
        p_[p_port] = colon < host_port.size() ? p_[p_host_end] + 1 : pos;
        unsigned value = 0;
        for (size_t i = p_[p_port]; i != pos; ++i) {
            if (url[i] < '0' || url[i] > '9'
                || (value = value * 10 + (url[i] - '0')) > 65535) {
                return false;
            }
        }
    }

    // /path?search#hash
    p_[p_path] = pos;
    while (pos != n && (cclass(url[pos]) & us_safe)) {
        ++pos;
    }
    p_[p_search] = pos;
    if (pos != n && url[pos] == '?') {
        do {
            ++pos;
        } while (pos != n && (cclass(url[pos]) & us_query));
    }
    p_[p_hash] = pos;
    if (pos != n && url[pos] == '#') {
        do {
            ++pos;
        } while (pos != n && (cclass(url[pos]) & us_fragment));
    }
    if (pos != n) {             // a character URLs don't allow
        return false;
    }
    p_[p_end] = n;
    return true;
}

std::string http_unquote(std::string_view str) {
    if (str.empty() || str[0] != '\"') {
        return std::string(str);
    }
    const char* s = str.data() + 1;
    const char* last = str.data() + str.size();
    std::string acc;
    while (true) {
        const char* first = s;
        while (true) {
            if (s == last) {
                acc.append(first, s);
                return acc;
            } else if (*s == '\"') {
                acc.append(first, s);
                ++s;
                break;
            } else if (*s == '\\' && s + 1 != last) {
                acc.append(first, s);
                acc.push_back(s[1]);
                first = s + 2;
                s += 2;
            } else {
                ++s;
            }
        }

        first = s;
        while (s != last && *s != '\"') {
            ++s;
        }
        acc.append(first, s);
    }
}

} // namespace cotamer::strings
