#pragma once
#include "cotamer/cotamer.hh"
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

// cotamer/http_fields.hh
//    Iterators for HTTP field-value grammar (RFC 9110 §5.6) — comma-
//    separated lists (`#rule`) and semicolon-separated parameter lists.
//    Building blocks for parsing structured headers like `Connection`,
//    `Upgrade`, `Accept`, `Cache-Control`, `Sec-WebSocket-Extensions`.
//
//    Example:
//        if (auto c = req.header("Connection");
//            cot::strings::http_value_list(c).icontains("upgrade")) ...
//
//    Also `url_view`, which splits a URL into its parts.

namespace cotamer {
class http_message;
}

namespace cotamer::strings {

std::string http_unquote(std::string_view);


// View over a comma-separated header value (RFC 9110 §5.6.1 `#rule`).
// Supports forward iteration; each `*it` is a `string_view` with leading
// and trailing whitespace trimmed. Empty entries are skipped as HTTP semantics
// requires. A quoted-string entry (`"a,b"`) is preserved verbatim with its
// surrounding `"`s, is not split on embedded commas, and treats `\X` inside
// the quotes as an escape (pass the value to `http_unquote` to parse).
//
// `icontains` covers the common "is this token present?" check.
//
// The view is non-owning — the underlying header bytes must outlive it.

class http_value_list {
public:
    class iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using reference = std::string_view;
        using pointer = void;

        iterator() = default;
        iterator(std::string_view header) noexcept
            : rest_(header) {
            ++*this;
        }

        std::string_view operator*() const noexcept { return value_; }
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        constexpr bool operator==(const iterator& x) const noexcept {
            return value_.data() == x.value_.data();
        }

    private:
        std::string_view value_;
        std::string_view rest_;
    };

    explicit http_value_list(std::string_view header) noexcept
        : header_(header) {
    }
    explicit http_value_list(const char* header) noexcept
        : header_(header) {
    }

    iterator begin() const noexcept {
        return iterator(header_);
    }
    iterator end() const noexcept {
        const char* s = header_.data() + header_.size();
        return iterator({s, 0});
    }

    // Case-insensitive token-list membership test. Convenience for the
    // common `Connection: upgrade`, `Upgrade: websocket` style checks.
    bool icontains(std::string_view token) const noexcept {
        for (auto entry : *this) {
            if (strings::ieq(entry, token)) {
                return true;
            }
        }
        return false;
    }

private:
    std::string_view header_;
};


// View over a semicolon-separated parameter list (RFC 9110 §5.6.6), where each
// entry is either a bare token or `name=value` (`value` might be quoted).
// Supports forward iteration:
//
//     *it / span()    the entry's raw bytes, trimmed on both ends
//     name()          token left of `=`, or the whole span if no `=`
//     name_ieq(s)     case-insensitive name match
//     has_value()     true iff the entry contained a `=`
//     raw_value()     bytes right of `=` (with the surrounding `"`s
//                     intact for quoted values), or the whole span if
//                     no `=`
//     value()         `raw_value()` run through `http_unquote`
//
// The `=` parse is strict as RFC 9110 requires; `foo = bar` iterates as a
// single “parameter” whose `name()` is `foo = bar`. `foo=` (also invalid)
// has an empty value.
//
// Typically nested inside `http_value_list` (outer `#rule`, inner
// parameters) for headers like `Sec-WebSocket-Extensions`,
// `Content-Type`, `Cache-Control`.
//
// The view is non-owning — the underlying header bytes must outlive it.

class http_parameter_list {
public:
    class iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using reference = std::string_view;
        using pointer = void;

        iterator()
            : span_(), namelen_(0), rest_(span_) {
        }
        iterator(std::string_view header) noexcept
            : rest_(header) {
            ++*this;
        }

        std::string_view operator*() const noexcept { return span_; }
        iterator& operator++() noexcept;
        iterator operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        constexpr bool operator==(const iterator& x) const noexcept {
            return span_.data() == x.span_.data();
        }

        constexpr std::string_view span() const noexcept {
            return span_;
        }
        constexpr std::string_view name() const noexcept {
            return {span_.data(), namelen_};
        }
        bool name_ieq(std::string_view s) const noexcept {
            return strings::ieq(name(), s);
        }
        constexpr bool has_value() const noexcept {
            return namelen_ != span_.length();
        }
        std::string_view raw_value() const noexcept {
            if (namelen_ == span_.length()) {
                return span_;
            }
            return {span_.data() + namelen_ + 1, span_.length() - namelen_ - 1};
        }
        std::string value() const {
            return http_unquote(raw_value());
        }

    private:
        std::string_view span_;
        size_t namelen_;
        std::string_view rest_;
    };

    explicit http_parameter_list(std::string_view value) noexcept
        : value_(value) {
    }

    iterator begin() const noexcept {
        return iterator(value_);
    }
    iterator end() const noexcept {
        const char* s = value_.data() + value_.size();
        return iterator({s, 0});
    }

private:
    std::string_view value_;
};


// View of a URL’s parts, for absolute URLs (`scheme://authority/path?search#hash`)
// and origin-form URLs (`/path?search#hash`, as in most HTTP requests). For
// `https://u@[::1]:8443/a?b#c`:
//
//     scheme()      https           (empty for origin-form)
//     authority()   u@[::1]:8443
//     host_port()   [::1]:8443      (authority without user info)
//     host()        [::1]
//     port()        8443            (empty if absent)
//     path()        /a              (empty in `https://host?q`)
//     search()      ?b              (with the `?`, or empty)
//     hash()        #c              (with the `#`, or empty)
//
// `ok()` is false for other forms, for ports that aren't numbers up to 65535,
// and for characters URLs don't allow (like spaces); then every part is
// empty. Parts aren't percent-decoded. The URL must outlive the view.

class url_view {
public:
    explicit url_view(std::string_view url) noexcept;

    bool ok() const noexcept { return p_[p_end] != 0; }   // valid URLs aren't empty
    bool is_absolute() const noexcept { return p_[p_scheme_end] != 0; }
    std::string_view scheme() const noexcept { return part(0, p_[p_scheme_end]); }
    std::string_view authority() const noexcept { return part(p_[p_authority], p_[p_path]); }
    std::string_view host_port() const noexcept { return part(p_[p_host_port], p_[p_path]); }
    std::string_view host() const noexcept { return part(p_[p_host_port], p_[p_host_end]); }
    std::string_view port() const noexcept { return part(p_[p_port], p_[p_path]); }
    std::string_view path() const noexcept { return part(p_[p_path], p_[p_search]); }
    std::string_view search() const noexcept { return part(p_[p_search], p_[p_hash]); }
    std::string_view hash() const noexcept { return part(p_[p_hash], p_[p_end]); }

private:
    // part boundaries (offsets from `s_`), all 0 on failure
    enum {
        p_scheme_end, p_authority, p_host_port, p_host_end, p_port,
        p_path, p_search, p_hash, p_end, p_count
    };

    const char* s_;              // start of the URL
    uint32_t p_[p_count];

    bool parse(std::string_view url) noexcept;
    std::string_view part(uint32_t first, uint32_t last) const noexcept {
        return {s_ + first, last - first};
    }

    friend class cotamer::http_message;
};

} // namespace cotamer::strings


namespace cotamer::detail {
// Character classes for HTTP and URL parsing, shared by http.cc
constexpr uint16_t us_ows = 1;         // optional whitespace (space and tab)
constexpr uint16_t us_alpha = 2;       // ASCII letters
constexpr uint16_t us_hex = 4;         // hex digits
constexpr uint16_t us_safe = 8;        // URL-safe characters except ? and #
constexpr uint16_t us_header = 16;     // characters safe to include in HTTP headers
constexpr uint16_t us_http_tchar = 32; // characters matching HTTP `token`
constexpr uint16_t us_target = 64;     // request-target characters (us_header
                                       // except space and tab)
constexpr uint16_t us_scheme = 128;    // URL scheme characters
constexpr uint16_t us_query = 256;     // URL query characters (us_safe and ?)
constexpr uint16_t us_fragment = 512;  // URL fragment characters (us_query and #)

struct charinfo {
    uint16_t v[256];
};
extern const charinfo chartable;
} // namespace cotamer::detail
