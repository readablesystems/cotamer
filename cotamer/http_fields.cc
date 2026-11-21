#include "cotamer/http_fields.hh"

namespace cotamer::detail {

constinit const charinfo chartable = []{
    charinfo ct{};
    for (unsigned char ch = '0'; ch <= '9'; ++ch) {
        ct.v[ch] = us_safe | us_http_tchar | us_hex | ((ch - '0') << 4);
    }
    for (unsigned char ch = 'A'; ch <= 'F'; ++ch) {
        ct.v[ch] = ct.v[ch + 32] = us_safe | us_http_tchar | us_hex | ((ch - 'A' + 10) << 4);
    }
    for (unsigned char ch = 'G'; ch <= 'Z'; ++ch) {
        ct.v[ch] = ct.v[ch + 32] = us_safe | us_http_tchar;
    }
    for (auto s = "!#$%&'*+-.^_`|~"; *s; ++s) {
        ct.v[(unsigned char) *s] |= us_http_tchar;
    }
    for (auto s = "!$%&'()*+,-./:;=@[\\]^_`{|}~"; *s; ++s) {
        ct.v[(unsigned char) *s] |= us_safe;
    }
    for (unsigned char ch = 1; ch != 0; ++ch) {
        if (ch != '\r' && ch != '\n') {
            ct.v[ch] |= us_header;
        }
    }
    return ct;
}();

} // namespace cotamer::detail


namespace cotamer::strings {

auto http_value_list::iterator::operator++() noexcept -> iterator& {
    const char* s = rest_.data();
    const char* last = s + rest_.size();

    while (s != last && (*s == ' ' || *s == '\t' || *s == ',')) {
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
        if (*s == ' ' || *s == '\t') {
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

    while (s != last && (*s == ' ' || *s == '\t' || *s == ';')) {
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
        if (*s == ' ' || *s == '\t') {
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
        } else if (last_name == s && (detail::chartable.v[(unsigned char) *s] & detail::us_http_tchar)) {
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
